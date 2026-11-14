/* UMI's Android IMS DCM exchange and its dedicated WDS IPv6 bearer. */
#ifndef UMI_SDX55_IMS_H
#define UMI_SDX55_IMS_H

#include "umi_sdx55_wds.h"

#define SDX55_IMSDCM_SERVICE 770u
#define SDX55_IMSDCM_PORT 0x4309u
#define SDX55_IMS_WDS_PORT 0x430au
#define SDX55_IMS_WDS_MUX 3u
#define SDX55_IMS_PDP_ID 1u
#define SDX55_IMS_ACTIVATE 0x20u
#define SDX55_IMS_DEACTIVATE 0x21u

struct sdx55_ims_request {
	uint32_t sequence, instance;
	uint16_t txn, message;
	uint8_t profile, id;
};

struct sdx55_ims_wds_reply {
	uint32_t handle;
	uint16_t qmi_error;
	uint8_t address[16];
};

static inline int
sdx55_ims_dcm_request(const uint8_t *q, size_t n, struct sdx55_ims_request *request)
{
	if (n < 7 || q[0] != 0 || sdx55_cell_le16(q+5) != n-7) return EPROTO;
	struct sdx55_ims_request r = {.txn=sdx55_cell_le16(q+1), .message=sdx55_cell_le16(q+3)};
	if (r.message != SDX55_IMS_ACTIVATE && r.message != SDX55_IMS_DEACTIVATE) return ENOTSUP;
	bool have_params = false, have_sequence = false, have_instance = false;
	for (size_t off = 7; off < n; ) {
		if (n-off < 3) return EPROTO;
		unsigned tag = q[off];
		size_t len = sdx55_cell_le16(q+off+1);
		off += 3;
		if (len > n-off) return EPROTO;
		const uint8_t *p = q+off;
		off += len;
		if (tag == 1) {
			if (have_params) return EPROTO;
			have_params = true;
			if (r.message == SDX55_IMS_DEACTIVATE) {
				if (len != 1) return EPROTO;
				r.id = p[0];
				continue;
			}
			/* This modem requests the ims APN with family 1 (IPv6). The
			 * two final 16-bit fields are the 3GPP and 3GPP2 profile IDs. */
			if (len != 20 || p[0] != 3 || p[1] != 'i' || p[2] != 'm' || p[3] != 's' ||
			    sdx55_wds_get32(p+12) != 1 || sdx55_cell_le16(p+16) >= 255 ||
			    sdx55_cell_le16(p+18) != UINT16_MAX) return ENOTSUP;
			r.profile = p[16];
		} else if (tag == 0x10) {
			if (have_sequence || len != 4) return EPROTO;
			have_sequence = true;
			r.sequence = sdx55_wds_get32(p);
		} else if (tag == 0x13 && r.message == SDX55_IMS_ACTIVATE) {
			if (have_instance || len != 4) return EPROTO;
			have_instance = true;
			r.instance = sdx55_wds_get32(p);
		}
	}
	if (!have_params || !have_sequence || (r.message == SDX55_IMS_ACTIVATE && !have_instance)) return EPROTO;
	*request = r;
	return 0;
}

static inline size_t
sdx55_ims_dcm_reply(const struct sdx55_ims_request *r, bool indication, uint16_t txn,
    uint16_t error, const char *address, uint8_t *q, size_t capacity)
{
	/* At most the full 39-byte representation of an IPv6 address. */
	if (capacity < 80 || !sdx55_cell_empty_request(r->message, txn, q, capacity)) return 0;
	q[0] = indication ? 4 : 2;
	q[7] = 2; q[8] = 4; q[9] = 0;
	q[10] = !indication && error != 0; q[11] = 0;
	q[12] = (uint8_t)error; q[13] = (uint8_t)(error >> 8);
	size_t n = 14;
	if (!indication && error) { q[5] = 7; return n; }
	q[n++] = indication ? 1 : 0x10;
	q[n++] = 1; q[n++] = 0;
	q[n++] = r->message == SDX55_IMS_DEACTIVATE ? 0xfa : SDX55_IMS_PDP_ID;
	q[n++] = indication ? 0x10 : 0x11;
	q[n++] = 4; q[n++] = 0;
	sdx55_wds_put32(q+n, r->sequence); n += 4;
	if (indication && address && !error) {
		size_t len = 0;
		while (len < 40 && address[len]) len++;
		if (len != 39) return 0;
		q[n++] = 0x11; q[n++] = 44; q[n++] = 0;
		sdx55_wds_put32(q+n, 1); n += 4;
		q[n++] = (uint8_t)len;
		for (size_t i = 0; i < len; i++) q[n++] = (uint8_t)address[i];
	}
	if (r->message == SDX55_IMS_ACTIVATE) {
		q[n++] = 0x12; q[n++] = 4; q[n++] = 0;
		sdx55_wds_put32(q+n, r->instance); n += 4;
	}
	q[5] = (uint8_t)(n-7);
	return n;
}

static inline size_t
sdx55_ims_wds_request(uint16_t message, uint16_t txn, unsigned endpoint, unsigned argument,
    uint8_t *q, size_t cap)
{
	if (message == SDX55_WDS_BIND_SUBSCRIPTION)
		return sdx55_cell_bind_request(message, txn, argument, q, cap);
	if (message != SDX55_WDS_START) {
		size_t n = sdx55_wds_request(message, txn, endpoint, SDX55_IMS_WDS_MUX, argument, q, cap);
		if (n && message == SDX55_WDS_SET_FAMILY) q[10] = 6;
		return n;
	}
	/* Same WDS Start fields as UMI imsd: APN, IPv6, profiles, embedded client. */
	const uint8_t fields[] = {0x14,3,0,'i','m','s', 0x19,1,0,6,
	    0x31,1,0,(uint8_t)argument, 0x32,1,0,255, 0x35,1,0,1};
	if (argument >= 255 || cap < 7+sizeof(fields) || !sdx55_cell_empty_request(message, txn, q, cap)) return 0;
	q[5] = sizeof(fields);
	for (size_t i = 0; i < sizeof(fields); i++) q[7+i] = fields[i];
	return 7+sizeof(fields);
}

static inline int
sdx55_ims_wds_decode(uint16_t message, uint16_t txn, const uint8_t *q, size_t n, struct sdx55_ims_wds_reply *out)
{
	*out = (struct sdx55_ims_wds_reply){0};
	int error = sdx55_cell_body(message, txn, q, n, &out->qmi_error);
	if (error) return error;
	bool found = false;
	for (size_t off = 7; off < n; ) {
		unsigned tag = q[off];
		size_t len = sdx55_cell_le16(q+off+1);
		const uint8_t *p = q+off+3;
		off += 3+len;
		if (message == SDX55_WDS_START && tag == 1) {
			if (found || len != 4) return EPROTO;
			out->handle = sdx55_wds_get32(p);
			found = true;
		} else if (message == SDX55_WDS_SETTINGS && tag == 0x25) {
			if (found || len != 17 || !p[16] || p[16] > 128) return EPROTO;
			if ((p[0] & 0xe0) != 0x20 && (p[0] & 0xfe) != 0xfc) return EADDRNOTAVAIL;
			for (size_t i = 0; i < sizeof(out->address); i++) out->address[i] = p[i];
			found = true;
		}
	}
	if (message == SDX55_WDS_START && (!out->handle || out->handle == UINT32_MAX)) return EPROTO;
	if (message == SDX55_WDS_SETTINGS && !found) return EADDRNOTAVAIL;
	return 0;
}

#endif
