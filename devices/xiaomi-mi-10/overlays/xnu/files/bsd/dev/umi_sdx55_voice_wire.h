/* VOICE, IMSA and IMS Settings QMI layouts from libqmi and Qualcomm voice_service_v02. */
#ifndef UMI_SDX55_VOICE_WIRE_H
#define UMI_SDX55_VOICE_WIRE_H

#include "umi_sdx55_cell.h"
#include "../sys/umi_sdx55_voice.h"

#define SDX55_VOICE_SERVICE 9u
#define SDX55_IMSS_SERVICE 18u
#define SDX55_IMSA_SERVICE 33u
#define SDX55_VOICE_REGISTER 0x03u
#define SDX55_VOICE_DIAL 0x20u
#define SDX55_VOICE_END 0x21u
#define SDX55_VOICE_ALL_STATUS 0x2eu
#define SDX55_VOICE_GET_ALL 0x2fu
#define SDX55_VOICE_BIND 0x44u
#define SDX55_VOICE_CODEC 0x53u
#define SDX55_IMSA_REGISTRATION 0x20u
#define SDX55_IMSA_SERVICES 0x21u
#define SDX55_IMSA_BIND 0x33u
#define SDX55_IMSA_GET_BIND 0x34u
#define SDX55_IMSS_GET_ENABLED 0x90u
#define SDX55_IMSS_BIND 0x98u
#define SDX55_VOICE_PORT 0x4306u
#define SDX55_IMSA_PORT 0x4307u
#define SDX55_IMSS_PORT 0x4308u
#define SDX55_VOICE_MAX_CALLS 7u
#define SDX55_DIAL_AUTOMATIC 2u

struct sdx55_voice_call {
	uint8_t id, state, type, direction, mode;
	uint16_t reason;
	bool matches_number;
};

struct sdx55_voice_result {
	uint16_t qmi_error, reason;
	uint8_t id, count;
	bool has_id;
	uint32_t value, technology;
	uint32_t ims_enabled, volte_enabled;
	struct sdx55_voice_call calls[SDX55_VOICE_MAX_CALLS];
};

static inline size_t
sdx55_voice_request(unsigned service, uint16_t message, uint16_t txn, unsigned argument,
    const char *number, uint8_t *out, size_t capacity)
{
	if (!sdx55_cell_empty_request(message, txn, out, capacity)) return 0;
	if (service == SDX55_VOICE_SERVICE && message == SDX55_VOICE_REGISTER) {
		if (capacity < 11) return 0;
		out[5] = 4; out[7] = 0x15; out[8] = 1; out[9] = 0; out[10] = 1;
		return 11;
	}
	if (service == SDX55_VOICE_SERVICE && message == SDX55_VOICE_DIAL) {
		/* The working UMI Android dial requests automatic service, with
		 * bidirectional audio, zero video and the stock defaults. */
		static const uint8_t automatic[] = {
			0x16, 4, 0, 1, 0, 0, 0,
			0x18, 8, 0, 3, 0, 0, 0, 0, 0, 0, 0,
			0x19, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0x22, 1, 0, 0,
			0x23, 4, 0, 0, 0, 0, 0,
		};
		unsigned n = number ? umi_voice_number_length(number) : 0;
		unsigned extra = argument == SDX55_DIAL_AUTOMATIC ? sizeof(automatic) : 0;
		if (!n || capacity < 14 + n + extra || (argument != 0 && argument != SDX55_DIAL_AUTOMATIC)) return 0;
		out[5] = n + 7 + extra;
		out[7] = 1; out[8] = n; out[9] = 0;
		for (unsigned i = 0; i < n; i++) out[10+i] = number[i];
		out[10+n] = 0x10; out[11+n] = 1; out[12+n] = 0; out[13+n] = 0;
		for (unsigned i = 0; i < extra; i++) out[14+n+i] = automatic[i];
		return 14 + n + extra;
	}
	if (service == SDX55_VOICE_SERVICE && (message == SDX55_VOICE_END || message == SDX55_VOICE_BIND)) {
		if (capacity < 11 || argument > UINT8_MAX) return 0;
		if (message == SDX55_VOICE_BIND && (argument < 1 || argument > 2)) return 0;
		out[5] = 4; out[7] = 1; out[8] = 1; out[9] = 0;
		out[10] = message == SDX55_VOICE_BIND ? argument - 1 : argument;
		return 11;
	}
	if ((service == SDX55_IMSA_SERVICE && message == SDX55_IMSA_BIND) ||
	    (service == SDX55_IMSS_SERVICE && message == SDX55_IMSS_BIND)) {
		if (capacity < 14 || argument < 1 || argument > 2) return 0;
		out[5] = 7; out[7] = service == SDX55_IMSA_SERVICE ? 0x10 : 1; out[8] = 4; out[9] = 0;
		/* The IMS services use a zero-based subscription in a 32-bit field. */
		out[10] = argument - 1; out[11] = out[12] = out[13] = 0;
		return 14;
	}
	if ((service == SDX55_VOICE_SERVICE && message == SDX55_VOICE_GET_ALL) ||
	    (service == SDX55_IMSA_SERVICE && (message == SDX55_IMSA_REGISTRATION || message == SDX55_IMSA_SERVICES ||
	    message == SDX55_IMSA_GET_BIND)) || (service == SDX55_IMSS_SERVICE && message == SDX55_IMSS_GET_ENABLED)) return 7;
	return 0;
}

static inline struct sdx55_voice_call *
sdx55_voice_find(struct sdx55_voice_result *result, uint8_t id)
{
	for (unsigned i = 0; i < result->count; i++)
		if (result->calls[i].id == id) return &result->calls[i];
	return NULL;
}

static inline int
sdx55_voice_list(struct sdx55_voice_result *out, const uint8_t *calls, size_t size,
    const uint8_t *reasons, size_t reason_size, const uint8_t *numbers, size_t number_size, const char *target)
{
	if (!calls) return 0; /* GET_ALL has an optional list when idle. */
	if (!size || calls[0] > SDX55_VOICE_MAX_CALLS || size != 1 + 7u * calls[0]) return EPROTO;
	for (unsigned i = 0; i < calls[0]; i++) {
		const uint8_t *c = calls + 1 + 7*i;
		if (sdx55_voice_find(out, c[0])) return EPROTO;
		out->calls[out->count++] = (struct sdx55_voice_call){.id=c[0], .state=c[1],
		    .type=c[2], .direction=c[3], .mode=c[4]};
	}
	if (reasons) {
		if (!reason_size || reason_size != 1 + 3u * reasons[0]) return EPROTO;
		for (unsigned i = 0; i < reasons[0]; i++) {
			const uint8_t *r = reasons + 1 + 3*i;
			struct sdx55_voice_call *call = sdx55_voice_find(out, r[0]);
			if (call) call->reason = sdx55_cell_le16(r+1);
		}
	}
	if (numbers && target) {
		if (!number_size) return EPROTO;
		unsigned target_length = umi_voice_number_length(target);
		size_t offset = 1;
		for (unsigned i = 0; i < numbers[0]; i++) {
			if (number_size - offset < 3) return EPROTO;
			const uint8_t *n = numbers + offset;
			if (n[2] > number_size - offset - 3) return EPROTO;
			struct sdx55_voice_call *call = sdx55_voice_find(out, n[0]);
			if (call && target_length && n[2] == target_length) {
				bool match = true;
				for (unsigned j = 0; j < target_length; j++) match &= n[3+j] == (uint8_t)target[j];
				call->matches_number = match;
			}
			offset += 3 + n[2];
		}
		if (offset != number_size) return EPROTO;
	}
	return 0;
}

static inline int
sdx55_voice_decode(unsigned service, uint16_t message, uint16_t txn, const uint8_t *packet,
    size_t size, const char *target, struct sdx55_voice_result *result)
{
	struct sdx55_voice_result decoded = {.value=UINT32_MAX, .technology=UINT32_MAX,
	    .ims_enabled=UINT32_MAX, .volte_enabled=UINT32_MAX};
	bool indication = service == SDX55_VOICE_SERVICE && message == SDX55_VOICE_ALL_STATUS;
	int error = 0;
	if (indication) {
		if (size < 7 || packet[0] != 4 || sdx55_cell_le16(packet+3) != message ||
		    sdx55_cell_le16(packet+5) != size - 7) return EPROTO;
	} else {
		error = sdx55_cell_body(message, txn, packet, size, &decoded.qmi_error);
		if (error && error != EIO) return error;
	}
	const uint8_t *calls = NULL, *reasons = NULL, *numbers = NULL;
	size_t call_size = 0, reason_size = 0, number_size = 0;
	for (size_t offset = 7; offset < size; ) {
		if (size - offset < 3) return EPROTO;
		unsigned tag = packet[offset];
		size_t n = sdx55_cell_le16(packet+offset+1);
		if (n > size - offset - 3) return EPROTO;
		const uint8_t *data = packet + offset + 3;
		offset += 3 + n;
		if (service == SDX55_IMSA_SERVICE && message == SDX55_IMSA_REGISTRATION && tag == 0x10 &&
		    n == 1 && decoded.value == UINT32_MAX && data[0] <= 1) {
			decoded.value = data[0] ? 2 : 0; /* older IMSA registration boolean */
		} else if (service == SDX55_IMSA_SERVICE && message == SDX55_IMSA_REGISTRATION && tag == 0x11 && n == 2) {
			decoded.reason = sdx55_cell_le16(data);
		} else if (service == SDX55_IMSA_SERVICE && n == 4) {
			if ((message == SDX55_IMSA_REGISTRATION && tag == 0x12) ||
			    (message == SDX55_IMSA_SERVICES && tag == 0x11) || (message == SDX55_IMSA_GET_BIND && tag == 0x10))
				decoded.value = sdx55_cell_le16(data) | (uint32_t)sdx55_cell_le16(data+2) << 16;
			if (tag == 0x14) decoded.technology = sdx55_cell_le16(data) | (uint32_t)sdx55_cell_le16(data+2) << 16;
		} else if (service == SDX55_IMSS_SERVICE && message == SDX55_IMSS_GET_ENABLED && n == 1) {
			if (tag == 0x11) decoded.volte_enabled = data[0];
			if (tag == 0x19) decoded.ims_enabled = data[0];
		} else if (service == SDX55_VOICE_SERVICE && (message == SDX55_VOICE_DIAL || message == SDX55_VOICE_END)) {
			if (tag == 0x10) {
				if (decoded.has_id || n != 1) return EPROTO;
				decoded.has_id = true;
				decoded.id = data[0];
			}
			if (message == SDX55_VOICE_DIAL && tag == 0x14 && n == 2) decoded.reason = sdx55_cell_le16(data);
		} else if (service == SDX55_VOICE_SERVICE && (indication || message == SDX55_VOICE_GET_ALL)) {
			if (tag == (indication ? 1 : 0x10)) { if (calls) return EPROTO; calls = data; call_size = n; }
			if (tag == (indication ? 0x14 : 0x18)) { if (reasons) return EPROTO; reasons = data; reason_size = n; }
			if (tag == (indication ? 0x10 : 0x11)) { if (numbers) return EPROTO; numbers = data; number_size = n; }
		}
	}
	if (!error) {
		if (indication && !calls) return EPROTO;
		error = sdx55_voice_list(&decoded, calls, call_size, reasons, reason_size, numbers, number_size, target);
	}
	*result = decoded;
	return error;
}

#endif
