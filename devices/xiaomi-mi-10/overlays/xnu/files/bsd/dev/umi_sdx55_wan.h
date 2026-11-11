/* SDX55 WDS status and WDA data format on QRTR. Message IDs are service-scoped. */
#ifndef UMI_SDX55_WAN_H
#define UMI_SDX55_WAN_H

#include "umi_sdx55_cell.h"

#define SDX55_WDS_GET_STATUS 0x0022u
#define SDX55_WDA_SET_FORMAT 0x0020u
#define SDX55_WDA_GET_FORMAT 0x0021u

struct sdx55_wan_status {
	uint16_t service_error;
	uint16_t unsupported_option; /* WDA option blocking a reversible format change; zero if none. */
	uint8_t connection_status; /* WDS status on this client's port */
	/* Optional WDA fields. UINT32_MAX means the modem omitted the field. */
	uint32_t link_layer, uplink, downlink;
	uint32_t dl_max_datagrams, dl_max_size, ul_max_datagrams, ul_max_size;
};

static inline size_t
sdx55_wan_request(uint16_t message, uint16_t txn, void *out, size_t capacity)
{
	if (message != SDX55_WDS_GET_STATUS && message != SDX55_WDA_GET_FORMAT) return 0;
	return sdx55_cell_empty_request(message, txn, out, capacity);
}

/* WDA Get Data Format, endpoint-info TLV. Qualcomm data_ep_type_enum_v01
 * assigns 3 to PCIe. The interface number is separate from a QMAP mux ID. */
static inline size_t
sdx55_wda_pcie_format_request(uint16_t txn, uint32_t endpoint_id, void *out, size_t capacity)
{
	if (!out || capacity < 18) return 0;
	uint8_t *packet = out;
	sdx55_cell_empty_request(SDX55_WDA_GET_FORMAT, txn, packet, capacity);
	packet[5] = 11;
	packet[7] = 0x10;
	packet[8] = 8;
	packet[9] = 0;
	packet[10] = 3;
	packet[11] = packet[12] = packet[13] = 0;
	for (unsigned i = 0; i < 4; i++) packet[14 + i] = (uint8_t)(endpoint_id >> (8 * i));
	return 18;
}


/* These are the values read on UMI stock Android, rmnet_mhi0 on PCIe
 * endpoint 4. The bounded host transport supports this QMAPv5 profile. */
static inline struct sdx55_wan_status
sdx55_wda_stock_format(void)
{
	return (struct sdx55_wan_status){
		.link_layer = 2, .uplink = 9, .downlink = 9,
		.dl_max_datagrams = 31, .dl_max_size = 31668,
		.ul_max_datagrams = 32, .ul_max_size = 16384,
	};
}

static inline int
sdx55_wda_format_equal(const struct sdx55_wan_status *a, const struct sdx55_wan_status *b)
{
	return a && b && !a->service_error && !b->service_error &&
	    !a->unsupported_option && !b->unsupported_option &&
	    a->link_layer == b->link_layer && a->uplink == b->uplink && a->downlink == b->downlink &&
	    a->dl_max_datagrams == b->dl_max_datagrams && a->dl_max_size == b->dl_max_size &&
	    a->ul_max_datagrams == b->ul_max_datagrams && a->ul_max_size == b->ul_max_size;
}

static inline int
sdx55_wda_format_supported(const struct sdx55_wan_status *s)
{
	struct sdx55_wan_status stock = sdx55_wda_stock_format();
	if (sdx55_wda_format_equal(s, &stock)) return 1;
	if (!s || s->service_error || s->unsupported_option ||
	    s->link_layer != 2 || s->uplink != 0 || s->downlink != 0) return 0;
	/* In the disabled cold profile unused limits can be absent or zero. */
	const uint32_t limits[] = { s->dl_max_datagrams, s->dl_max_size, s->ul_max_datagrams, s->ul_max_size };
	for (unsigned i = 0; i < sizeof(limits) / sizeof(limits[0]); i++)
		if (limits[i] != 0 && limits[i] != UINT32_MAX) return 0;
	return 1;
}

/* WDA SET request tags differ from the response tags. Only the two known
 * profiles above can be written. Capture a supported scoped GET before SET
 * and use a fresh scoped GET to verify each change, including restoration. */
static inline size_t
sdx55_wda_set_pcie_format_request(uint16_t txn, uint32_t endpoint_id,
    const struct sdx55_wan_status *s, void *out, size_t capacity)
{
	if (endpoint_id != 4 || !sdx55_wda_format_supported(s)) return 0;
	const struct { uint8_t tag; uint32_t value; } fields[] = {
		{ 0x11, s->link_layer }, { 0x12, s->uplink }, { 0x13, s->downlink },
		{ 0x15, s->dl_max_datagrams }, { 0x16, s->dl_max_size }, { 0x17, 3 },
		{ 0x1b, s->ul_max_datagrams }, { 0x1c, s->ul_max_size },
	};
	uint8_t body[60];
	size_t n = 0;
	for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		if (fields[i].value == UINT32_MAX) continue;
		body[n++] = fields[i].tag;
		body[n++] = fields[i].tag == 0x17 ? 8 : 4;
		body[n++] = 0;
		for (unsigned b = 0; b < 4; b++) body[n++] = (uint8_t)(fields[i].value >> (8 * b));
		if (fields[i].tag == 0x17) {
			for (unsigned b = 0; b < 4; b++) body[n++] = (uint8_t)(endpoint_id >> (8 * b));
		}
	}
	uint8_t *packet = out;
	if (capacity < n + 7 || !sdx55_cell_empty_request(SDX55_WDA_SET_FORMAT, txn, packet, capacity))
		return 0;
	packet[5] = (uint8_t)n;
	for (size_t i = 0; i < n; i++) packet[7 + i] = body[i];
	return 7 + n;
}

/* SET may return the QMI operation result alone. Read back the scoped GET
 * even if the firmware echoed selected optional fields in this reply. */
static inline int
sdx55_wda_set_ack(uint16_t txn, const void *packet, size_t size, uint16_t *service_error)
{
	return sdx55_cell_body(SDX55_WDA_SET_FORMAT, txn, packet, size, service_error);
}

/* libqmi data/qmi-service-wda.json, Get Data Format response. */
static inline uint32_t *
sdx55_wan_format_field(struct sdx55_wan_status *status, uint8_t tag)
{
	switch (tag) {
	case 0x11: return &status->link_layer;
	case 0x12: return &status->uplink;
	case 0x13: return &status->downlink;
	case 0x15: return &status->dl_max_datagrams;
	case 0x16: return &status->dl_max_size;
	case 0x17: return &status->ul_max_datagrams;
	case 0x18: return &status->ul_max_size;
	default: return NULL;
	}
}

static inline int
sdx55_wan_decode(uint16_t message, uint16_t txn, const void *buffer, size_t size,
    struct sdx55_wan_status *out)
{
	if (!out || (message != SDX55_WDS_GET_STATUS && message != SDX55_WDA_GET_FORMAT)) return EINVAL;
	const uint8_t *packet = buffer;
	struct sdx55_wan_status status = {
		.connection_status = UINT8_MAX,
		.link_layer = UINT32_MAX, .uplink = UINT32_MAX, .downlink = UINT32_MAX,
		.dl_max_datagrams = UINT32_MAX, .dl_max_size = UINT32_MAX,
		.ul_max_datagrams = UINT32_MAX, .ul_max_size = UINT32_MAX,
	};
	int error = sdx55_cell_body(message, txn, packet, size, &status.service_error);
	if (error) {
		if (error == EIO) *out = status;
		return error;
	}
	/* sdx55_cell_body checked the whole envelope, including unknown TLVs. */
	unsigned seen = 0;
	for (size_t off = 7; off < size; ) {
		uint8_t tag = packet[off];
		size_t n = sdx55_cell_le16(packet + off + 1);
		const uint8_t *value = packet + off + 3;
		off += 3 + n;
		if (message == SDX55_WDS_GET_STATUS) {
			if (tag != 1) continue;
			if (seen || n != 1) return EINVAL;
			status.connection_status = value[0];
			seen = 1;
		} else {
			if (tag == 2) continue;
			uint32_t *field = sdx55_wan_format_field(&status, tag);
			size_t width;
			if (field || tag == 0x14 || tag == 0x1a) width = 4;
			else if (tag == 0x10 || tag == 0x1b) width = 1;
			else {
				/* Retain the diagnostic GET. Mutating an unrecognised
				 * format would leave no verifiable restore recipe. */
				status.unsupported_option = tag ? tag : 256;
				continue;
			}
			unsigned bit = 1u << (tag - 0x10);
			if ((seen & bit) || n != width) return EINVAL;
			seen |= bit;
			uint32_t v = width == 1 ? value[0] :
			    (uint32_t)sdx55_cell_le16(value) | ((uint32_t)sdx55_cell_le16(value + 2) << 16);
			if (field) *field = v;
			/* Optional QoS, NDP signature, padding and flow control are
			 * unsupported when active. An explicit 0xffffffff is invalid
			 * for managed fields and must not masquerade as an absent TLV. */
			if ((field && v == UINT32_MAX) || (!field && v)) status.unsupported_option = tag;
		}
	}
	if (message == SDX55_WDS_GET_STATUS && !seen) return EINVAL;
	*out = status;
	return 0;
}

#endif
