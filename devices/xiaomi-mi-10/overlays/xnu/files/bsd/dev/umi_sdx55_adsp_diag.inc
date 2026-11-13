/* Optional voice diagnostics on the UMI LPASS QRTR transport. Only counters
 * and control metadata leave this receiver. Encoded speech and PCM stay on the device. */
#include "umi_sdx55_voice_diag_wire.h"

#define ADSP_DIAG_SERVICE 4097u
#define ADSP_DIAG_CONTROL_PORT 16460u
#define ADSP_DIAG_DATA_PORT 16432u
#define ADSP_DIAG_LOG_ITEMS 2481u
#define ADSP_DIAG_MASK_BYTES ((ADSP_DIAG_LOG_ITEMS + 7u) / 8u)
#define ADSP_DIAG_ID_SUPPORT (1u << 15)
#define ADSP_DIAG_ID_V2 (1u << 19)

struct voice_diag_pcm_level {
	uint64_t energy;
	int64_t sum;
	unsigned samples, clipped;
};

/* The QRTR receiver owns these statistics. Keep the DSP clock so bursts of
 * DIAG traffic don't shift the comparison between speech and playback. */
static struct {
	unsigned generation;
	uint64_t second;
	struct voice_diag_pcm_level mic[2], stream_tx, decoder;
	unsigned tx_empty, tx_sid, tx_other;
} voice_diag_levels;

static struct {
	bool enabled, features_sent, mask_sent, hdlc_escape;
	uint32_t node, control, data, features;
	uint8_t root_id, process_count;
	char processes[8][30];
	unsigned mask_attempts, boot_messages, media;
	unsigned call_generation, message_generation, call_messages;
	uint64_t call_message_keys[64];
	unsigned ctrl_packets, datagrams, apr_records, modem_apr, tx_frames, rx_frames;
	unsigned decode_periods, decode_audible, cvp_periods, cvp_audible;
	unsigned afe_periods, afe_audible;
	unsigned handset_rx, handset_rx_audible, handset_tx, handset_tx_audible;
	unsigned cvp_tx, cvp_tx_audible;
	unsigned hdlc_size;
	uint8_t hdlc[2048];
} voice_diag;

static void
voice_diag_message_mask(uint16_t first, uint16_t last)
{
	struct {
		uint32_t type, size;
		uint8_t stream, status, mode;
		uint16_t first, last;
		uint32_t count, masks[25];
	} __attribute__((packed)) mask = {11, 0, 1, 3, 0, first, last, last - first + 1u, {0}};
	if (mask.count > sizeof(mask.masks) / sizeof(mask.masks[0])) return;
	for (unsigned i = 0; i < mask.count; i++) mask.masks[i] = 0x1c; /* high, error, fatal */
	mask.size = 11 + mask.count * sizeof(mask.masks[0]);
	(void)qrtr_out_packet(QRTR_DATA, ADSP_DIAG_CONTROL_PORT, voice_diag.node,
	    voice_diag.control, &mask, 8 + mask.size);
}

static uint32_t
voice_diag_word(const uint8_t *data)
{
	uint32_t word;
	memcpy(&word, data, sizeof(word));
	return word;
}

static uint64_t
voice_diag_clock(const uint8_t *data)
{
	uint64_t clock;
	memcpy(&clock, data, sizeof(clock));
	return clock;
}

static unsigned
voice_diag_pcm_rms(const struct voice_diag_pcm_level *level)
{
	if (!level->samples) return 0;
	/* Remove DC before measuring the speech level. */
	uint64_t sum = level->sum < 0 ? -level->sum : level->sum;
	uint64_t mean = sum / level->samples, rest = sum % level->samples;
	uint64_t dc = mean * mean + 2 * mean * rest / level->samples;
	uint64_t power = level->energy / level->samples;
	power -= MIN(power, dc);
	unsigned lo = 0, hi = 32769;
	while (hi - lo > 1) {
		unsigned mid = (lo + hi) / 2;
		if (mid * mid <= power) lo = mid;
		else hi = mid;
	}
	return lo;
}

static bool
voice_diag_level_window(uint64_t clock)
{
	unsigned generation = os_atomic_load(&voice_diag.call_generation, acquire);
	if (!(generation & 1u)) return false;
	uint64_t second = (clock >> 16) / 800u; /* DIAG ticks are 1.25 ms. */
	if (voice_diag_levels.generation == generation) {
		if (second < voice_diag_levels.second) return false;
		if (second == voice_diag_levels.second) return true;
		const struct voice_diag_pcm_level *mic = voice_diag_levels.mic;
		const struct voice_diag_pcm_level *tx = &voice_diag_levels.stream_tx;
		const struct voice_diag_pcm_level *rx = &voice_diag_levels.decoder;
		tell("UMI SDX55: voice levels dsp_sec=%llu mic_rms=%u,%u tx_rms=%u rx_rms=%u samples=%u,%u,%u,%u clipped=%u,%u,%u tx_bytes_1_6_other=%u,%u,%u\n",
		    voice_diag_levels.second, voice_diag_pcm_rms(&mic[0]), voice_diag_pcm_rms(&mic[1]),
		    voice_diag_pcm_rms(tx), voice_diag_pcm_rms(rx), mic[0].samples, mic[1].samples, tx->samples,
		    rx->samples, mic[0].clipped, mic[1].clipped, tx->clipped, voice_diag_levels.tx_empty,
		    voice_diag_levels.tx_sid, voice_diag_levels.tx_other);
	}
	bzero(&voice_diag_levels, sizeof(voice_diag_levels));
	voice_diag_levels.generation = generation;
	voice_diag_levels.second = second;
	return true;
}

static void
voice_diag_level_add(struct voice_diag_pcm_level *level, int16_t sample)
{
	level->sum += sample;
	level->energy += (int32_t)sample * sample;
	level->samples++;
	level->clipped += (sample >= 32760 || sample <= -32760);
}

void
hakios_adsp_voice_diag_call(bool active)
{
	if (!voice_diag.enabled) return;
	unsigned generation = os_atomic_load(&voice_diag.call_generation, relaxed);
	/* Odd generations denote a call. The DIAG receiver owns its log budget. */
	if ((bool)(generation & 1u) != active)
		os_atomic_store(&voice_diag.call_generation, generation + 1u, release);
}

static void
voice_diag_announce(uint32_t node)
{
	if (!voice_diag.enabled) return;
	voice_diag.node = node;
	const struct qrtr_control control = {QRTR_NEW_SERVER, {ADSP_DIAG_SERVICE, 64, QRTR_US, ADSP_DIAG_CONTROL_PORT}};
	const struct qrtr_control data = {QRTR_NEW_SERVER, {ADSP_DIAG_SERVICE, 66, QRTR_US, ADSP_DIAG_DATA_PORT}};
	int ctrl_error = qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, node, QRTR_CONTROL, &control, sizeof(control));
	int data_error = qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, node, QRTR_CONTROL, &data, sizeof(data));
	tell("UMI SDX55: voice DSP DIAG advertised control=%d data=%d\n", ctrl_error, data_error);
}

static void
voice_diag_process(const uint8_t *body, unsigned size)
{
	if (size < 9) return;
	uint32_t version = voice_diag_word(body);
	if (version != 1 && version != 2) return;
	const uint8_t *name = body + 8;
	unsigned available = size - 8;
	if (version == 2) {
		if (available < 4) return;
		unsigned features = voice_diag_word(name);
		if (features > available - 4) return;
		name += 4 + features;
		available -= 4 + features;
	}
	const uint8_t *end = memchr(name, 0, MIN(available, sizeof(voice_diag.processes[0])));
	if (!end || end == name) return;
	unsigned name_bytes = (unsigned)(end - name) + 1;
	unsigned slot;
	for (slot = 0; slot < voice_diag.process_count; slot++) {
		if (!memcmp(voice_diag.processes[slot], name, name_bytes)) break;
	}
	if (slot >= sizeof(voice_diag.processes) / sizeof(voice_diag.processes[0])) return;
	if (slot == voice_diag.process_count) {
		memcpy(voice_diag.processes[slot], name, name_bytes);
		voice_diag.process_count++;
	}
	/* DIAG reserves 0 for unknown and 1 for the application processor.
	 * The request's ID field is undefined until we assign it. */
	uint32_t process_id = slot + 2;
	struct { uint32_t type, size, version, id; uint8_t name[30]; } reply = {
		33, 8 + name_bytes, 1, process_id, {0}
	};
	memcpy(reply.name, name, name_bytes);
	int error = qrtr_out_packet(QRTR_DATA, ADSP_DIAG_CONTROL_PORT, voice_diag.node,
	    voice_diag.control, &reply, 16 + name_bytes);
	bool root = strnstr(voice_diag.processes[slot], "root", name_bytes - 1) != NULL;
	if (!error && root) voice_diag.root_id = process_id;
	tell("UMI SDX55: voice DSP DIAG process id=%u root=%u errno=%d\n", process_id, root, error);
}

static void
voice_diag_control(const struct qrtr_header *header, const uint8_t *data)
{
	if (voice_diag.control && voice_diag.control != header->from_port) return;
	voice_diag.control = header->from_port;
	os_atomic_inc(&voice_diag.ctrl_packets, relaxed);
	for (unsigned offset = 0; header->size - offset >= 8;) {
		uint32_t id = voice_diag_word(data + offset), size = voice_diag_word(data + offset + 4);
		if (size > header->size - offset - 8) break;
		const uint8_t *body = data + offset + 8;
		offset += 8 + size;
		if (!voice_diag.mask_sent) tell("UMI SDX55: voice DSP DIAG control opcode=%u bytes=%u\n", id, size);
		if (id == 8 && size >= 5 && !voice_diag.features_sent) {
			unsigned feature_bytes = MIN(voice_diag_word(body), MIN(size - 4, 4u));
			uint32_t peer = 0;
			memcpy(&peer, body + 4, feature_bytes);
			const uint32_t supported = (1u << 0) | (1u << 2) | (1u << 4) | (1u << 6) |
			    (1u << 11) | (1u << 13) | (1u << 14) | ADSP_DIAG_ID_SUPPORT | (peer & ADSP_DIAG_ID_V2);
			const uint32_t features[] = {8, 8, 4, supported};
			int error = qrtr_out_packet(QRTR_DATA, ADSP_DIAG_CONTROL_PORT, voice_diag.node,
			    voice_diag.control, features, sizeof(features));
			voice_diag.features_sent = !error;
			voice_diag.features = peer;
			tell("UMI SDX55: voice DSP DIAG peer features=%#x errno=%d\n", peer, error);
		}
		if (id == 33) voice_diag_process(body, size);
	}
	if (voice_diag.features_sent && (!(voice_diag.features & ADSP_DIAG_ID_SUPPORT) || voice_diag.root_id) &&
	    !voice_diag.mask_sent && voice_diag.mask_attempts < 4) {
		voice_diag.mask_attempts++;
		/* Include APR events to verify the data channel before a call. */
		static const uint16_t codes[] = {0x14d2, 0x1586, 0x158a, 0x158b, 0x1804, 0x1805};
		struct {
			uint32_t type, size;
			uint8_t stream, status, equipment;
			uint32_t items, bytes;
			uint8_t mask[ADSP_DIAG_MASK_BYTES];
		} __attribute__((packed)) logs = {9, 11 + ADSP_DIAG_MASK_BYTES, 1, 3, 1,
			ADSP_DIAG_LOG_ITEMS, ADSP_DIAG_MASK_BYTES, {0}};
		for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
			unsigned index = codes[i] - 0x1000u;
			logs.mask[index / 8] |= 1u << (index % 8);
		}
		int error = qrtr_out_packet(QRTR_DATA, ADSP_DIAG_CONTROL_PORT, voice_diag.node,
		    voice_diag.control, &logs, sizeof(logs));
		voice_diag.mask_sent = !error;
		tell("UMI SDX55: voice DSP DIAG voice mask errno=%d\n", error);
		const uint32_t realtime[11] = {3, 36, 1, 1, 1};
		(void)qrtr_out_packet(QRTR_DATA, ADSP_DIAG_CONTROL_PORT, voice_diag.node,
		    voice_diag.control, realtime, sizeof(realtime));
		voice_diag_message_mask(8500, 8524); /* QDSP6 audio, voice and AFE */
	}
}

static void
voice_diag_apr(const uint8_t *apr, unsigned size, uint64_t clock)
{
	struct sdx55_diag_voice_apr packet;
	if (!sdx55_diag_voice_apr_read(apr, size, &packet)) return;
	if (packet.opcode == SDX55_DIAG_APR_SET_MEDIA && packet.body_bytes >= 8)
		os_atomic_store(&voice_diag.media, voice_diag_word(packet.body + 4), relaxed);
	if (voice_diag.modem_apr++ < SDX55_DIAG_APR_TRACE_LIMIT)
		sdx55_diag_voice_apr_trace("DSP", &packet);
	/* These fixed-size voice timing and path-delay controls contain no PCM. */
	switch (packet.opcode) {
	case 0x1325d:
	case 0x12e66:
	case 0x1334c:
		if (packet.body_bytes == 8 || packet.body_bytes == 12)
			tell("UMI SDX55: voice DSP timing opcode=%#x clock=%#llx words=%#x,%#x,%#x\n",
			    packet.opcode, clock, voice_diag_word(packet.body), voice_diag_word(packet.body + 4),
			    packet.body_bytes == 12 ? voice_diag_word(packet.body + 8) : 0);
		break;
	default:
		break;
	}
}

static void
voice_diag_message(const uint8_t *data, unsigned length)
{
	if (length < 24) return;
	unsigned ssid = data[14] | (unsigned)data[15] << 8;
	if (ssid < 8500 || ssid > 8524) return;
	unsigned line = data[12] | (unsigned)data[13] << 8;
	unsigned generation = os_atomic_load(&voice_diag.call_generation, acquire);
	bool in_call = (generation & 1u) != 0;
	if (in_call) {
		if (voice_diag.message_generation != generation) {
			voice_diag.message_generation = generation;
			voice_diag.call_messages = 0;
		}
		if (voice_diag.call_messages >= sizeof(voice_diag.call_message_keys) / sizeof(voice_diag.call_message_keys[0]))
			return;
		/* Deduplicate source lines and QShrink tokens without storing arguments. */
		uint32_t token = data[0] == 0x79 ? 0 : voice_diag_word(data + 20);
		uint64_t key = (uint64_t)ssid << 48 | (uint64_t)line << 32 | token;
		for (unsigned i = 0; i < voice_diag.call_messages; i++)
			if (voice_diag.call_message_keys[i] == key) return;
		voice_diag.call_message_keys[voice_diag.call_messages++] = key;
	} else {
		if (voice_diag.boot_messages >= 24) return;
		voice_diag.boot_messages++;
	}
	const char *phase = in_call ? "call" : "idle";
	uint64_t clock = voice_diag_clock(data + 4);
	if (data[0] == 0x79) {
		unsigned at = 20 + data[2] * 4u;
		if (at >= length) return;
		const uint8_t *format = data + at;
		const uint8_t *end = memchr(format, 0, length - at);
		if (!end || end + 1 == data + length) return;
		const char *file = (const char *)(end + 1);
		unsigned file_size = (unsigned)(data + length - end - 1);
		/* The firmware sends source templates and arguments. Report only
		 * bounded templates and filenames from the audio subsystem. */
		tell("UMI SDX55: voice DSP %s message clock=%#llx SSID=%u line=%u %.*s %.*s\n", phase, clock, ssid, line,
		    (int)MIN(file_size, 52u), file, (int)MIN((unsigned)(end - format), 140u), (const char *)format);
	} else if (length >= 24 + data[2] * 4u) {
		tell("UMI SDX55: voice DSP %s message clock=%#llx SSID=%u line=%u token=%#x args=%u\n", phase, clock, ssid, line,
		    voice_diag_word(data + 20), data[2]);
	}
}

static void
voice_diag_log(const uint8_t *data, unsigned length)
{
	if (length < 16) return;
	if (data[0] == 0x79 || data[0] == 0x92 || data[0] == 0x99) {
		voice_diag_message(data, length);
		return;
	}
	if (data[0] != 0x10) return;
	unsigned size = data[4] | (unsigned)data[5] << 8;
	if (size < 12 || size > length - 4) return;
	unsigned code = data[6] | (unsigned)data[7] << 8;
	if (code == 0x14d2) {
		os_atomic_inc(&voice_diag.apr_records, relaxed);
		voice_diag_apr(data + 16, size - 12, voice_diag_clock(data + 8));
	}
	uint64_t clock = voice_diag_clock(data + 8);
	if (code == 0x1804) {
		os_atomic_inc(&voice_diag.tx_frames, relaxed);
		/* The CVD header is 64 bytes; only count validated payload sizes. */
		if (size >= 12 + 64 && voice_diag_word(data + 36) == size - 12 - 64 &&
		    voice_diag_level_window(clock)) {
			unsigned bytes = size - 12 - 64;
			if (bytes == 1) voice_diag_levels.tx_empty++;
			else if (bytes == 6) voice_diag_levels.tx_sid++;
			else voice_diag_levels.tx_other++;
		}
	}
	if (code == 0x1805) {
		unsigned index = os_atomic_inc_orig(&voice_diag.rx_frames, relaxed);
		if (size >= 12 + 64 && (index < 2 || !(index % 100)))
			tell("UMI SDX55: voice DSP RX frame index=%u clock=%#llx bytes=%u sequence=%u speech_bytes=%u media=%#x decoder_media=%#x\n",
			    index + 1, clock, size - 12, voice_diag_word(data + 32), voice_diag_word(data + 36),
			    os_atomic_load(&voice_diag.media, relaxed), voice_diag_word(data + 74));
	}
	if ((code != 0x1586 && code != 0x158a && code != 0x158b) || size < 12 + 68) return;
	data += 16;
	unsigned offset = data[0] | (unsigned)data[1] << 8;
	if (offset < 68 || offset >= size - 12) return;
	uint32_t tap = voice_diag_word(data + 40);
	bool handset_rx = code == 0x1586 && tap == 0xb130;
	bool handset_tx = code == 0x1586 && tap == 0xb137;
	bool cvp_tx = code == 0x158a && tap == 0x10f60;
	bool stream_tx = code == 0x158b && tap == 1;
	bool decode = code == 0x158b && tap == 3;
	if ((code == 0x158b && !stream_tx && !decode) || (code == 0x158a && !cvp_tx && tap != 0x10f61) ||
	    (code == 0x1586 && !handset_rx && !handset_tx && tap != 0x8103)) return;
	bool audible = false;
	unsigned width = data[50];
	if (width != 16 && width != 32) return;
	unsigned sample_bytes = width / 8u;
	/* AFE and CVS records carry a channel count and packing flag. Only
	 * accept complete mono or stereo frames for the level measurements. */
	unsigned channels = data[48] | (unsigned)data[49] << 8;
	unsigned samples = (size - 12 - offset) / sample_bytes;
	struct voice_diag_pcm_level *level = NULL;
	if (channels == (handset_tx ? 2u : 1u) && samples && samples % channels == 0 &&
	    (size - 12 - offset) % sample_bytes == 0 && data[51] <= 1 &&
	    (handset_tx || stream_tx || decode) && voice_diag_level_window(clock)) {
		if (handset_tx) level = voice_diag_levels.mic;
		else level = stream_tx ? &voice_diag_levels.stream_tx : &voice_diag_levels.decoder;
	}
	unsigned frames = handset_tx ? samples / 2u : samples;
	for (unsigned i = 0; i < samples; i++) {
		/* Q31 taps carry the same signal scale as the signed Q15 taps. */
		unsigned high = offset + (i + 1) * sample_bytes - 2;
		int16_t sample = (int16_t)(data[high] | (uint16_t)data[high + 1] << 8);
		audible |= sample > 16 || sample < -16;
		if (level) {
			unsigned channel = handset_tx ? (data[51] ? i % 2u : i / frames) : 0;
			voice_diag_level_add(&level[channel], sample);
		}
	}
	if (stream_tx) return;
	if (decode) {
		os_atomic_inc(&voice_diag.decode_periods, relaxed);
		if (audible) os_atomic_inc(&voice_diag.decode_audible, relaxed);
	} else if (cvp_tx) {
		os_atomic_inc(&voice_diag.cvp_tx, relaxed);
		if (audible) os_atomic_inc(&voice_diag.cvp_tx_audible, relaxed);
	} else if (code == 0x158a) {
		os_atomic_inc(&voice_diag.cvp_periods, relaxed);
		if (audible) os_atomic_inc(&voice_diag.cvp_audible, relaxed);
	} else if (handset_rx) {
		os_atomic_inc(&voice_diag.handset_rx, relaxed);
		if (audible) os_atomic_inc(&voice_diag.handset_rx_audible, relaxed);
	} else if (handset_tx) {
		os_atomic_inc(&voice_diag.handset_tx, relaxed);
		if (audible) os_atomic_inc(&voice_diag.handset_tx_audible, relaxed);
	} else {
		os_atomic_inc(&voice_diag.afe_periods, relaxed);
		if (audible) os_atomic_inc(&voice_diag.afe_audible, relaxed);
	}
}

static void
voice_diag_data(const uint8_t *data, unsigned size)
{
	/* Apps-encoded frames have a length and start/end delimiters. Older
	 * firmware can send escaped HDLC instead, with records split across QRTR. */
	while (size >= 5 && data[0] == 0x7e && data[1] == 1) {
		unsigned bytes = data[2] | (unsigned)data[3] << 8;
		if (bytes > size - 5 || data[4 + bytes] != 0x7e) break;
		voice_diag_log(data + 4, bytes);
		data += 5 + bytes;
		size -= 5 + bytes;
	}
	if (!size) return;
	for (unsigned i = 0; i < size; i++) {
		uint8_t byte = data[i];
		if (byte == 0x7e) {
			if (!voice_diag.hdlc_escape && voice_diag.hdlc_size <= sizeof(voice_diag.hdlc))
				voice_diag_log(voice_diag.hdlc, voice_diag.hdlc_size);
			voice_diag.hdlc_size = 0;
			voice_diag.hdlc_escape = false;
		} else if (voice_diag.hdlc_size <= sizeof(voice_diag.hdlc)) {
			if (byte == 0x7d && !voice_diag.hdlc_escape) {
				voice_diag.hdlc_escape = true;
				continue;
			}
			if (voice_diag.hdlc_size < sizeof(voice_diag.hdlc))
				voice_diag.hdlc[voice_diag.hdlc_size] = byte ^ (voice_diag.hdlc_escape ? 0x20 : 0);
			voice_diag.hdlc_size++;
			voice_diag.hdlc_escape = false;
		}
	}
}

static bool
voice_diag_input(const struct qrtr_header *header, const uint8_t *data)
{
	if (!voice_diag.enabled || header->from_node != voice_diag.node ||
	    header->to_node != QRTR_US || header->type != QRTR_DATA) return false;
	if (header->to_port == ADSP_DIAG_CONTROL_PORT) {
		voice_diag_control(header, data);
		return true;
	}
	if (header->to_port != ADSP_DIAG_DATA_PORT) return false;
	if (voice_diag.data && voice_diag.data != header->from_port) return true;
	voice_diag.data = header->from_port;
	if (!os_atomic_load(&voice_diag.datagrams, relaxed))
		tell("UMI SDX55: voice DSP DIAG first data packet size=%u\n", header->size);
	os_atomic_inc(&voice_diag.datagrams, relaxed);
	voice_diag_data(data, header->size);
	return true;
}

void
hakios_adsp_voice_diag_report(void)
{
	if (!voice_diag.enabled) return;
	tell("UMI SDX55: voice DSP DIAG control=%u packets=%u APR=%u frames RX=%u TX=%u decoded=%u audible=%u CVP=%u audible=%u AFE record=%u audible=%u handset RX=%u audible=%u TX=%u audible=%u CVP TX=%u audible=%u\n",
	    os_atomic_load(&voice_diag.ctrl_packets, relaxed), os_atomic_load(&voice_diag.datagrams, relaxed),
	    os_atomic_load(&voice_diag.apr_records, relaxed),
	    os_atomic_load(&voice_diag.rx_frames, relaxed), os_atomic_load(&voice_diag.tx_frames, relaxed),
	    os_atomic_load(&voice_diag.decode_periods, relaxed), os_atomic_load(&voice_diag.decode_audible, relaxed),
	    os_atomic_load(&voice_diag.cvp_periods, relaxed), os_atomic_load(&voice_diag.cvp_audible, relaxed),
	    os_atomic_load(&voice_diag.afe_periods, relaxed), os_atomic_load(&voice_diag.afe_audible, relaxed),
	    os_atomic_load(&voice_diag.handset_rx, relaxed), os_atomic_load(&voice_diag.handset_rx_audible, relaxed),
	    os_atomic_load(&voice_diag.handset_tx, relaxed), os_atomic_load(&voice_diag.handset_tx_audible, relaxed),
	    os_atomic_load(&voice_diag.cvp_tx, relaxed), os_atomic_load(&voice_diag.cvp_tx_audible, relaxed));
}
