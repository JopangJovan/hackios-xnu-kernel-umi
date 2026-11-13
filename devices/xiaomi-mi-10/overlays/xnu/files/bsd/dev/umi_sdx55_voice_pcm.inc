/* Qualcomm CVS downlink at stream end -> voice RX pseudoport -> ADM -> ASM.
 * Session 3 is independent of media playback (1) and the microphone (2).
 */
#define VOICE_PCM_AFE 4u
#define VOICE_PCM_ASM 7u
#define VOICE_PCM_ADM 8u
#define VOICE_PCM_PORT 0x8003u
#define VOICE_PCM_SESSION 3u
#define VOICE_PCM_STREAM ((VOICE_PCM_SESSION << 8) | 1u)
#define VOICE_PCM_RATE 48000u
#define VOICE_PCM_PERIODS (UMI_VOICE_PCM_BYTES / 4096u)
#define VOICE_PCM_PERIOD_BYTES (VOICE_PCM_RATE / 50u * sizeof(int16_t))
#define VOICE_PCM_READ 0x00010dacu
#define VOICE_PCM_READ_DONE 0x00010d9au
#define VOICE_PCM_TOKEN 0x20000000u
#define VOICE_PCM_DONE 0x10000000u
#define VOICE_PCM_ERROR 0x40000000u
#define VOICE_PCM_OFFSET_MASK 0xfffu

static struct {
	bool wanted, running;
	uint16_t source, cvs;
} voice_pcm_worker;
static uint32_t voice_pcm_serial, voice_pcm_period[VOICE_PCM_PERIODS];

struct sdx55_voice_pcm {
	uintptr_t memory;
	uint64_t address;
	uint32_t mapping;
	uint16_t copp;
	bool port, stream, device, tap;
};

static bool
sdx55_voice_pcm_event(uint8_t service, __unused uint16_t port, uint32_t opcode,
    uint32_t token, const uint32_t *words, uint32_t length)
{
	if (service != VOICE_PCM_ASM || (token & 0xf0000000u) != VOICE_PCM_TOKEN) return false;
	unsigned period = token & (VOICE_PCM_PERIODS - 1u);
	uint32_t result = VOICE_PCM_ERROR;
	if (opcode == VOICE_PCM_READ_DONE && length >= 24) {
		uint32_t bytes = words[4], offset = words[5];
		if (words[0] == 0 && !(offset & 1u) && !(bytes & 1u) && offset <= VOICE_PCM_PERIOD_BYTES &&
		    bytes <= VOICE_PCM_PERIOD_BYTES - offset)
			result = VOICE_PCM_DONE | (bytes << 12) | offset;
		else result |= words[0] & 0xffffu;
	} else if (opcode == 0x000100beu || (opcode == SDX55_APR_RESULT && length >= 8 &&
	    words[0] == VOICE_PCM_READ && words[1] == 0)) {
		return true; /* Accepted. The DMA completion follows separately. */
	} else if (opcode == SDX55_APR_RESULT && length >= 8) {
		result |= words[1] & 0xffffu;
	}
	/* The token changes at every submission, including across calls. */
	os_atomic_cmpxchg(&voice_pcm_period[period], token, result, release);
	return true;
}

static bool
sdx55_voice_pcm_read(const struct sdx55_voice_pcm *pcm, unsigned period)
{
	uint64_t address = pcm->address + 4096u * period;
	uint32_t token = VOICE_PCM_TOKEN | ((++voice_pcm_serial << 2) & 0x0ffffffcu) | period;
	const uint32_t read[] = { (uint32_t)address, (uint32_t)(address >> 32), pcm->mapping,
		VOICE_PCM_PERIOD_BYTES, voice_pcm_serial };
	os_atomic_store(&voice_pcm_period[period], token, release);
	return hakios_adsp_apr(VOICE_PCM_ASM, VOICE_PCM_STREAM, token, VOICE_PCM_READ, read, sizeof(read), 0, NULL);
}

static bool
sdx55_voice_pcm_command(uint8_t service, uint16_t port, uint32_t opcode, const void *payload, unsigned size)
{
	return !sdx55_voice_audio_command(service, port, port, opcode, payload, size, NULL);
}

static bool
sdx55_voice_pcm_setup(struct sdx55_voice_pcm *pcm)
{
	const struct {
		uint16_t port, size;
		uint32_t memory[3], module, parameter;
		uint16_t parameter_size, reserved;
		uint32_t version;
		uint16_t bits, channels, format, timing;
		uint32_t rate;
	} __attribute__((packed)) port_config = {
		.port=VOICE_PCM_PORT, .size=28, .module=0x1020c, .parameter=0x10219, .parameter_size=16,
		.version=1, .bits=16, .channels=1, .timing=1, .rate=VOICE_PCM_RATE
	};
	const uint16_t port[] = { VOICE_PCM_PORT, 0 };
	if (!sdx55_voice_pcm_command(VOICE_PCM_AFE, 0, 0x100ef, &port_config, sizeof(port_config)) ||
	    !sdx55_voice_pcm_command(VOICE_PCM_AFE, 0, 0x100e5, port, sizeof(port))) return false;
	pcm->port = true;
	if (!hakios_audio_voice_pcm_memory(&pcm->memory, &pcm->address)) return false;
	const struct {
		uint16_t pool, regions;
		uint32_t property, low, high, bytes;
	} __attribute__((packed)) map = {
		3, 1, 0, (uint32_t)pcm->address, (uint32_t)(pcm->address >> 32), UMI_VOICE_PCM_BYTES
	};
	struct hakios_apr_answer answer = {0};
	if (!hakios_adsp_apr(VOICE_PCM_ASM, 0, 0, 0x10d92, &map, sizeof(map), 400, &answer) ||
	    answer.opcode != 0x10d93 || answer.length < 4 || !answer.words[0]) {
		hakios_tl("UMI SDX55: voice PCM map reply=%#x value=%u status=%u\n", answer.opcode, answer.words[0], answer.words[1]);
		return false;
	}
	pcm->mapping = answer.words[0];
	const struct {
		uint32_t mode, endpoint, topology, format;
		uint16_t bits, reserved;
	} __attribute__((packed)) stream = { 0, 0, 0x10c68, 0x10da5, 16, 0 };
	if (!sdx55_voice_pcm_command(VOICE_PCM_ASM, VOICE_PCM_STREAM, 0x10db4, &stream, sizeof(stream))) return false;
	pcm->stream = true;
	const struct {
		uint32_t param, size, frames_per_buffer, block_size;
		uint16_t channels, bits;
		uint32_t rate;
		uint16_t is_signed, reserved;
		uint8_t channel_map[8];
	} __attribute__((packed)) encoder = { 0x10da3, 28, 0, 20, 1, 16, VOICE_PCM_RATE, 1, 0, { 3 } };
	if (!sdx55_voice_pcm_command(VOICE_PCM_ASM, VOICE_PCM_STREAM, 0x10c10, &encoder, sizeof(encoder))) return false;
	const struct {
		uint16_t flags, mode, endpoint1, endpoint2;
		uint32_t topology;
		uint16_t channels, bits;
		uint32_t rate;
		uint8_t channel_map[8];
	} __attribute__((packed)) device = { 0, 2, VOICE_PCM_PORT, 0xffff, 0x10312, 1, 16, VOICE_PCM_RATE, { 3 } };
	if (!hakios_adsp_apr(VOICE_PCM_ADM, VOICE_PCM_PORT, 0, 0x10326, &device, sizeof(device), 400, &answer) ||
	    answer.opcode != 0x10329 || answer.length < 8 || answer.words[0] != 0 || (uint16_t)answer.words[1] == 0xff) {
		hakios_tl("UMI SDX55: voice PCM device reply=%#x value=%u status=%u\n", answer.opcode, answer.words[0], answer.words[1]);
		return false;
	}
	pcm->copp = (uint16_t)answer.words[1];
	pcm->device = true;
	const struct {
		uint32_t matrix, sessions;
		uint16_t session, copps, copp[2];
	} __attribute__((packed)) route = { 1, 1, VOICE_PCM_SESSION, 1, { pcm->copp, 0 } };
	if (!sdx55_voice_pcm_command(VOICE_PCM_ADM, 0, 0x10325, &route, sizeof(route))) return false;
	if (!os_atomic_load(&voice_pcm_worker.wanted, acquire)) return false;
	const struct {
		uint32_t rx_tap, tx_tap;
		uint16_t port;
		uint32_t mode;
	} __attribute__((packed)) tap = { 0x10f79, 0x10f78, 0xffff, 0 };
	if (sdx55_voice_audio_command(SDX55_APR_CVS, voice_pcm_worker.source, voice_pcm_worker.cvs,
	    0x112be, &tap, sizeof(tap), NULL)) return false;
	pcm->tap = true;
	const uint32_t run[] = { 0, 0, 0 };
	return sdx55_voice_pcm_command(VOICE_PCM_ASM, VOICE_PCM_STREAM, 0x10daa, run, sizeof(run));
}

static void
sdx55_voice_pcm_close(const struct sdx55_voice_pcm *pcm)
{
	hakios_audio_voice_flush();
	if (pcm->tap)
		(void)sdx55_voice_audio_command(SDX55_APR_CVS, voice_pcm_worker.source, voice_pcm_worker.cvs,
		    0x11237, NULL, 0, NULL);
	if (pcm->stream) (void)sdx55_voice_pcm_command(VOICE_PCM_ASM, VOICE_PCM_STREAM, 0x10bcd, NULL, 0);
	if (pcm->device) (void)sdx55_voice_pcm_command(VOICE_PCM_ADM, pcm->copp, 0x10327, NULL, 0);
	if (pcm->port) {
		const uint16_t port[] = { VOICE_PCM_PORT, 0 };
		(void)sdx55_voice_pcm_command(VOICE_PCM_AFE, 0, 0x100e6, port, sizeof(port));
	}
	if (pcm->mapping) (void)sdx55_voice_pcm_command(VOICE_PCM_ASM, 0, 0x10d94, &pcm->mapping, sizeof(pcm->mapping));
	if (pcm->memory) hakios_audio_voice_pcm_memory_put();
}

static void
sdx55_voice_pcm_feed(const struct sdx55_voice_pcm *pcm)
{
	for (unsigned i = 0; i < VOICE_PCM_PERIODS; i++)
		if (!sdx55_voice_pcm_read(pcm, i)) return;

	uint64_t report, timeout;
	clock_interval_to_deadline(500, NSEC_PER_MSEC, &report);
	clock_interval_to_deadline(2, NSEC_PER_SEC, &timeout);
	unsigned next = 0, reads = 0, errors = 0, nonzero = 0, frames = 0, played = 0;
	while (os_atomic_load(&voice_pcm_worker.wanted, acquire)) {
		uint64_t now = mach_absolute_time();
		if (now >= report) {
			sdx55_voice_audio_observe_mailbox("during call");
			hakios_tl("UMI SDX55: voice downlink reads=%u errors=%u nonzero_periods=%u frames=%u queued=%u\n",
			    reads, errors, nonzero, frames, played);
			clock_interval_to_deadline(2, NSEC_PER_SEC, &report);
		}
		uint32_t result = os_atomic_load(&voice_pcm_period[next], acquire);
		if (!(result & (VOICE_PCM_DONE | VOICE_PCM_ERROR))) {
			if (now >= timeout) break;
			delay_for_interval(2, NSEC_PER_MSEC);
			continue;
		}
		if (result & VOICE_PCM_ERROR) {
			errors++;
			hakios_tl("UMI SDX55: voice downlink DMA completion failed status=%u\n", result & 0xffffu);
			break;
		}
		uintptr_t page = pcm->memory + 4096u * next;
		for (uintptr_t line = page; line < page + VOICE_PCM_PERIOD_BYTES; line += 64)
			__asm__ volatile ("dc ivac, %0" : : "r" (line));
		__asm__ volatile ("dsb sy" ::: "memory");
		const int16_t *samples = (const int16_t *)(page + (result & VOICE_PCM_OFFSET_MASK));
		unsigned count = ((result & ~VOICE_PCM_DONE) >> 12) / sizeof(*samples);
		bool audible = false;
		for (unsigned i = 0; i < count; i++) audible |= samples[i] > 16 || samples[i] < -16;
		nonzero += audible;
		frames += count;
		played += hakios_audio_voice_play(samples, count);
		reads++;
		if (!sdx55_voice_pcm_read(pcm, next)) break;
		next = (next + 1) % VOICE_PCM_PERIODS;
		clock_interval_to_deadline(2, NSEC_PER_SEC, &timeout);
	}
	hakios_tl("UMI SDX55: voice downlink stopped reads=%u errors=%u nonzero_periods=%u frames=%u queued=%u\n",
	    reads, errors, nonzero, frames, played);
}

#include "umi_sdx55_voice_cvp.inc"

static void
sdx55_voice_pcm_thread(__unused void *arg, __unused wait_result_t wait_result)
{
	if (voice_audio.microphone && os_atomic_load(&voice_pcm_worker.wanted, acquire))
		hakios_mic_voice_activate();
	if (!sdx55_voice_cvp_pcm() && os_atomic_load(&voice_pcm_worker.wanted, acquire)) {
		struct sdx55_voice_pcm pcm = {0};
		hakios_adsp_apr_events_voice(sdx55_voice_pcm_event);
		bool ready = sdx55_voice_pcm_setup(&pcm);
		hakios_tl("UMI SDX55: voice downlink ready=%u\n", ready);
		if (ready && os_atomic_load(&voice_pcm_worker.wanted, acquire))
			sdx55_voice_pcm_feed(&pcm);
		sdx55_voice_pcm_close(&pcm);
	}
	os_atomic_store(&voice_pcm_worker.running, false, release);
	thread_terminate(current_thread());
}

static void
sdx55_voice_audio_connected(void)
{
	if (!voice_audio.started || os_atomic_load(&voice_pcm_worker.running, acquire)) return;
	voice_pcm_worker.source = voice_audio.source;
	voice_pcm_worker.cvs = voice_audio.cvs;
	os_atomic_store(&voice_pcm_worker.wanted, true, release);
	os_atomic_store(&voice_pcm_worker.running, true, release);
	thread_t thread;
	if (kernel_thread_start(sdx55_voice_pcm_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
	else os_atomic_store(&voice_pcm_worker.running, false, release);
}

static void
sdx55_voice_pcm_stop(void)
{
	os_atomic_store(&voice_pcm_worker.wanted, false, release);
	while (os_atomic_load(&voice_pcm_worker.running, acquire)) delay_for_interval(2, NSEC_PER_MSEC);
}
