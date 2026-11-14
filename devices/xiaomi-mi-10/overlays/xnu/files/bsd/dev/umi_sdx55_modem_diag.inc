/* Stock SDX55 DIAG channel, enabled by the voice diagnostics boot argument.
 * Only the voice log mask is sent. Log headers and counters are retained. */
#include "umi_sdx55_voice_diag_wire.h"

static struct {
	uint8_t frame[512];
	unsigned length;
	bool escaped;
	unsigned packets, tx, rx, apr, voice_apr, acknowledged;
} modem_diag;

static uint16_t
modem_diag_crc(const uint8_t *packet, size_t length)
{
	uint16_t crc = 0xffff;
	for (size_t i = 0; i < length; i++) {
		crc ^= packet[i];
		for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
	}
	return crc ^ 0xffff;
}

static void
sdx55_modem_diag_log(const uint8_t *packet, unsigned size)
{
	if (size < 3) return;
	unsigned length = size - 2;
	if (modem_diag_crc(packet, length) != (packet[length] | (unsigned)packet[length + 1] << 8)) return;
	os_atomic_inc(&modem_diag.packets, relaxed);
	if (packet[0] == 0x98 && length >= 9) {
		packet += 8;
		length -= 8;
	}
	if (packet[0] == 0x73 && length >= 12 && packet[4] == 3) {
		unsigned status = packet[8] | (unsigned)packet[9] << 8;
		if (!status) os_atomic_inc(&modem_diag.acknowledged, relaxed);
		hakios_tl("UMI SDX55: voice modem DIAG mask status=%u\n", status);
	}
	if (packet[0] != 0x10 || length < 16) return;
	unsigned log_bytes = packet[4] | (unsigned)packet[5] << 8;
	if (log_bytes < 12 || log_bytes > length - 4) return;
	unsigned log = packet[6] | (unsigned)packet[7] << 8;
	if (log == 0x1914) os_atomic_inc(&modem_diag.tx, relaxed);
	if (log == 0x1915) {
		unsigned index = os_atomic_inc_orig(&modem_diag.rx, relaxed);
		if (log_bytes >= 64 && (index < 2 || !(index % 100))) {
			uint32_t sequence, media;
			memcpy(&sequence, packet + 40, sizeof(sequence));
			memcpy(&media, packet + 64, sizeof(media));
			hakios_tl("UMI SDX55: voice modem RX frame index=%u sequence=%u media=%#x\n", index + 1, sequence, media);
		}
	}
	if (log != 0x14d0) return;
	os_atomic_inc(&modem_diag.apr, relaxed);
	struct sdx55_diag_voice_apr apr;
	if (!sdx55_diag_voice_apr_read(packet + 16, log_bytes - 12, &apr)) return;
	if (modem_diag.voice_apr++ < SDX55_DIAG_APR_TRACE_LIMIT)
		sdx55_diag_voice_apr_trace("modem", &apr);
	if (apr.opcode == SDX55_DIAG_APR_MAILBOX && apr.body_bytes >= 20) {
		uint64_t dsp, pcie;
		uint32_t bytes;
		memcpy(&dsp, apr.body, sizeof(dsp));
		memcpy(&pcie, apr.body + 8, sizeof(pcie));
		memcpy(&bytes, apr.body + 16, sizeof(bytes));
		hakios_tl("UMI SDX55: voice modem received mailbox DSP=%#llx PCIe=%#llx size=%#x\n", dsp, pcie, bytes);
	}
}

static void
sdx55_modem_diag_receive(__unused void *context, const void *payload, size_t size)
{
	const uint8_t *data = payload;
	for (size_t i = 0; i < size; i++) {
		uint8_t byte = data[i];
		if (byte == 0x7e) {
			if (!modem_diag.escaped && modem_diag.length <= sizeof(modem_diag.frame))
				sdx55_modem_diag_log(modem_diag.frame, modem_diag.length);
			modem_diag.length = 0;
			modem_diag.escaped = false;
		} else if (modem_diag.length <= sizeof(modem_diag.frame)) {
			if (byte == 0x7d && !modem_diag.escaped) {
				modem_diag.escaped = true;
				continue;
			}
			if (modem_diag.length < sizeof(modem_diag.frame))
				modem_diag.frame[modem_diag.length] = byte ^ (modem_diag.escaped ? 0x20 : 0);
			modem_diag.length++;
			modem_diag.escaped = false;
		}
	}
}

static int
sdx55_modem_diag_start(struct umi_mhi *host)
{
	if (!sdx55_voice_diag_enabled()) return 0;
	int error = umi_mhi_sdx55_diag_start(host);
	if (error) return error;
	/* 3424 equipment-1 items is the range reported by this SDX55 image. */
	enum { items = 3424, bytes = (items + 7) / 8 };
	struct { uint32_t type, op, equip, count; uint8_t mask[bytes]; } __attribute__((packed)) request = {
		0x73, 3, 1, items, {0}
	};
	static const uint16_t codes[] = {0x14d0, 0x1914, 0x1915};
	for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		unsigned code = codes[i] - 0x1000;
		request.mask[code / 8] |= 1u << (code % 8);
	}
	uint16_t crc = modem_diag_crc((const uint8_t *)&request, sizeof(request));
	uint8_t encoded[2 * (sizeof(request) + 2) + 1];
	unsigned count = 0;
	for (unsigned i = 0; i < sizeof(request) + 2; i++) {
		uint8_t byte = i < sizeof(request) ? ((const uint8_t *)&request)[i] : crc >> (8 * (i - sizeof(request)));
		if (byte == 0x7e || byte == 0x7d) {
			encoded[count++] = 0x7d;
			byte ^= 0x20;
		}
		encoded[count++] = byte;
	}
	encoded[count++] = 0x7e;
	return umi_mhi_sdx55_diag_send(host, encoded, count);
}

static void
sdx55_modem_diag_report(void)
{
	if (!sdx55_voice_diag_enabled()) return;
	hakios_tl("UMI SDX55: voice modem DIAG mask_ack=%u packets=%u APR=%u frames RX=%u TX=%u\n",
	    os_atomic_load(&modem_diag.acknowledged, relaxed), os_atomic_load(&modem_diag.packets, relaxed),
	    os_atomic_load(&modem_diag.apr, relaxed), os_atomic_load(&modem_diag.rx, relaxed),
	    os_atomic_load(&modem_diag.tx, relaxed));
}
