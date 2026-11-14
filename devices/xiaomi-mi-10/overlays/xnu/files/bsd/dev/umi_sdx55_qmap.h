/* SDX55 QMAPv5 on the PCIe IP data channels. Wire layout follows the UMI
 * Android rmnet_map_header and rmnet_map_v5_csum_header. The MAP packet
 * length includes tail padding, but excludes both headers. */
#ifndef UMI_SDX55_QMAP_H
#define UMI_SDX55_QMAP_H

#include <stdint.h>
#include <stddef.h>

enum sdx55_qmap_kind {
	SDX55_QMAP_SKIP, SDX55_QMAP_IPV4,
	SDX55_QMAP_FLOW_ENABLE, SDX55_QMAP_FLOW_DISABLE,
};

struct sdx55_qmap_frame {
	enum sdx55_qmap_kind kind;
	/* Borrowed until the underlying MHI buffer is reposted. */
	const uint8_t *ip;
	size_t ip_length;
	/* Complete unpadded command echo, owned by this result. */
	uint8_t ack[20];
};

static inline uint16_t
sdx55_qmap_be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

/* The BSD interface uses software checksums. Leave transport checks to the
 * IP stack, including reassembly of fragments before transport processing. */
static inline int
sdx55_qmap_ipv4(const uint8_t *p, size_t length)
{
	if (!p || length < 20 || (p[0] >> 4) != 4 || sdx55_qmap_be16(p + 2) != length)
		return 0;
	size_t header = (p[0] & 15u) * 4u;
	if (header < 20 || header > length) return 0;
	uint32_t sum = 0;
	for (size_t i = 0; i < header; i += 2) sum += sdx55_qmap_be16(p + i);
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return sum == 0xffff;
}

/* Encode one already checksummed datagram. Input may alias the output. */
static inline size_t
sdx55_qmap_encode(uint8_t mux, const uint8_t *ip, size_t length, uint8_t *out, size_t cap)
{
	if (!out || !mux || mux == UINT8_MAX || cap < 8 || length > cap - 8 ||
	    !sdx55_qmap_ipv4(ip, length))
		return 0;
	__builtin_memmove(out + 8, ip, length);
	out[0] = 0x40;
	out[1] = mux;
	out[2] = (uint8_t)(length >> 8);
	out[3] = (uint8_t)length;
	out[4] = 4; /* checksum extension type 2, last header */
	out[5] = out[6] = out[7] = 0;
	return 8 + length;
}

/* Decode one record. Advance by the return value even for a skipped record.
 * A zero return means there is no trustworthy boundary for another record
 * in this MHI transfer. In particular, Qualcomm hands a coalesced descriptor
 * to its segmentation engine whole, and its length cannot be used to skip
 * it as though it were an ordinary checksum header. */
static inline size_t
sdx55_qmap_next(uint8_t mux, const uint8_t *data, size_t remaining, struct sdx55_qmap_frame *f)
{
	if (!f) return 0;
	*f = (struct sdx55_qmap_frame){ .kind = SDX55_QMAP_SKIP };
	if (!data || remaining < 4) return 0;
	size_t length = sdx55_qmap_be16(data + 2);
	size_t padding = data[0] & 0x3fu;
	size_t header = 4;
	int command = (data[0] & 0x80u) != 0;
	if (data[0] & 0x40u) {
		if (command || remaining < 8 || data[4] != 4) return 0;
		header = 8;
	}
	if (!length || length > remaining - header) return 0;
	size_t consumed = header + length;
	if (padding > length || !mux || mux == UINT8_MAX || data[1] != mux) return consumed;

	const uint8_t *payload = data + header;
	size_t bytes = length - padding;
	if (command) {
		/* Flow disable/enable requests consist of the 8-byte command header
		 * and 8-byte flow descriptor. Preserve the opaque transaction, flow
		 * sequence and QoS ID. Only requests receive an ACK. */
		if (bytes != 16 || (payload[0] != 1 && payload[0] != 2) || payload[1] != 0)
			return consumed;
		f->kind = payload[0] == 1 ? SDX55_QMAP_FLOW_DISABLE : SDX55_QMAP_FLOW_ENABLE;
		f->ack[0] = 0x80;
		f->ack[1] = mux;
		f->ack[2] = 0;
		f->ack[3] = 16;
		__builtin_memcpy(f->ack + 4, payload, 16);
		f->ack[5] = 1; /* ACK */
	} else if (sdx55_qmap_ipv4(payload, bytes)) {
		f->kind = SDX55_QMAP_IPV4;
		f->ip = payload;
		f->ip_length = bytes;
	}
	return consumed;
}

#endif
