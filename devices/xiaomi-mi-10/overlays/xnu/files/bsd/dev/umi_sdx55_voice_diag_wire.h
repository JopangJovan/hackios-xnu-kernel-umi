#ifndef UMI_SDX55_VOICE_DIAG_WIRE_H
#define UMI_SDX55_VOICE_DIAG_WIRE_H

#include <libkern/libkern.h>

enum {
	SDX55_DIAG_APR_MODEM = 3,
	SDX55_DIAG_APR_DSP = 4,
	SDX55_DIAG_APR_RESULT = 0x110e8,
	SDX55_DIAG_APR_SET_MEDIA = 0x11186,
	SDX55_DIAG_APR_MAILBOX = 0x1333d,
	SDX55_DIAG_APR_TRACE_LIMIT = 256
};

struct sdx55_diag_voice_apr {
	const uint8_t *body;
	unsigned body_bytes;
	uint32_t opcode;
	uint16_t source_port, destination_port;
	uint8_t source_domain, destination_domain, source_service, destination_service;
};

static inline bool
sdx55_diag_voice_apr_read(const uint8_t *data, unsigned length, struct sdx55_diag_voice_apr *packet)
{
	if (length < 20) return false;
	unsigned header_bytes = (data[0] >> 4) * 4u;
	unsigned packet_bytes = data[2] | (unsigned)data[3] << 8;
	if (header_bytes < 20 || packet_bytes < header_bytes || packet_bytes > length) return false;
	uint8_t from = data[5], to = data[9];
	if (!((from == SDX55_DIAG_APR_MODEM && to == SDX55_DIAG_APR_DSP) ||
	    (from == SDX55_DIAG_APR_DSP && to == SDX55_DIAG_APR_MODEM))) return false;
	packet->body = data + header_bytes;
	packet->body_bytes = packet_bytes - header_bytes;
	packet->source_domain = from;
	packet->destination_domain = to;
	packet->source_service = data[4];
	packet->destination_service = data[8];
	packet->source_port = data[6] | (uint16_t)data[7] << 8;
	packet->destination_port = data[10] | (uint16_t)data[11] << 8;
	memcpy(&packet->opcode, data + 16, sizeof(packet->opcode));
	return true;
}

extern void hakios_tl(const char *, ...) __printflike(1, 2);

/* Retain only control metadata and replies. The body can contain speech. */
static inline void
sdx55_diag_voice_apr_trace(const char *observer, const struct sdx55_diag_voice_apr *packet)
{
	if (packet->opcode == SDX55_DIAG_APR_RESULT && packet->body_bytes >= 8) {
		uint32_t command, status;
		memcpy(&command, packet->body, sizeof(command));
		memcpy(&status, packet->body + 4, sizeof(status));
		hakios_tl("UMI SDX55: voice %s APR %u:%u:%#x->%u:%u:%#x result cmd=%#x status=%u\n",
		    observer, packet->source_domain, packet->source_service, packet->source_port,
		    packet->destination_domain, packet->destination_service, packet->destination_port, command, status);
	} else if (packet->opcode == SDX55_DIAG_APR_SET_MEDIA && packet->body_bytes >= 8) {
		uint32_t tx, rx;
		memcpy(&tx, packet->body, sizeof(tx));
		memcpy(&rx, packet->body + 4, sizeof(rx));
		hakios_tl("UMI SDX55: voice %s APR %u:%u:%#x->%u:%u:%#x set media TX=%#x RX=%#x\n",
		    observer, packet->source_domain, packet->source_service, packet->source_port,
		    packet->destination_domain, packet->destination_service, packet->destination_port, tx, rx);
	} else {
		hakios_tl("UMI SDX55: voice %s APR %u:%u:%#x->%u:%u:%#x opcode=%#x bytes=%u\n",
		    observer, packet->source_domain, packet->source_service, packet->source_port,
		    packet->destination_domain, packet->destination_service, packet->destination_port,
		    packet->opcode, packet->body_bytes);
	}
}

#endif
