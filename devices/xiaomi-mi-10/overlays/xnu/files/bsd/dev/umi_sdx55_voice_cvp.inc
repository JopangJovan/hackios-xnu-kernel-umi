/* Read the CVP's voice PCM tap directly using VSS_IVPCM, as the stock
 * msm-pcm-host-voice driver does. The first page contains the CVD map table.
 */
#define VOICE_CVP_RX_POINT 0x1128au
#define VOICE_CVP_START 0x11339u
#define VOICE_CVP_STOP 0x1100bu
#define VOICE_CVP_PUSH 0x1133au
#define VOICE_CVP_NOTIFY 0x1133bu
#define VOICE_CVP_MAP 0x11334u
#define VOICE_CVP_MAP_REPLY 0x11336u
#define VOICE_CVP_UNMAP 0x11337u
#define VOICE_CVP_OUTPUT 1u
#define VOICE_CVP_TICK 4u
#define VOICE_CVP_ERROR 8u
#define VOICE_CVP_PUSH_TOKEN 0x2fffffffu
#define VOICE_CVP_DMA_PATTERN 0x5a5a

static uint32_t voice_cvp_map_token, voice_cvp_map_result, voice_cvp_port, voice_cvp_notifies;
static uint64_t voice_cvp_notice, voice_cvp_buffer;

static bool
sdx55_voice_cvp_event(uint8_t service, uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *words, uint32_t length)
{
	if (service == SDX55_APR_MVM && token && token == os_atomic_load(&voice_cvp_map_token, acquire)) {
		if (opcode == VOICE_CVP_MAP_REPLY && length >= 4)
			os_atomic_store(&voice_cvp_map_result, words[0] ? words[0] : UINT32_MAX, release);
		else if (opcode == SDX55_APR_RESULT && length >= 8 && words[1])
			os_atomic_store(&voice_cvp_map_result, UINT32_MAX, release);
		return true;
	}
	if (service != SDX55_APR_CVP) return false;
	if (token == VOICE_CVP_PUSH_TOKEN) {
		if (opcode == SDX55_APR_RESULT && length >= 8 && words[1])
			os_atomic_store(&voice_cvp_notice, (uint64_t)VOICE_CVP_ERROR << 56, release);
		return true;
	}
	if (opcode != VOICE_CVP_NOTIFY) return false;
	uint32_t active_port = os_atomic_load(&voice_cvp_port, acquire);
	if (!active_port) return true;
	if (os_atomic_inc_orig(&voice_cvp_notifies, relaxed) == 0)
		hakios_tl("UMI SDX55: voice CVP first notify port=%u active=%u bytes=%u\n", port, active_port, length);
	if (length >= 32 && (port == active_port || !port) && words[0] == VOICE_CVP_RX_POINT) {
		uint64_t expected = os_atomic_load(&voice_cvp_buffer, acquire);
		uint64_t returned = (uint64_t)words[3] << 32 | words[2];
		uint16_t flags = words[1];
		if ((flags & VOICE_CVP_OUTPUT) && (!expected || (returned && returned != expected))) flags |= VOICE_CVP_ERROR;
		uint64_t notice = (uint64_t)flags << 56 | (uint64_t)(words[7] >> 16 & 0xff) << 48 |
		    (uint64_t)(words[7] & 0xffffu) << 32 | words[6];
		os_atomic_store(&voice_cvp_notice, notice, release);
	}
	return true;
}

static uint32_t
sdx55_voice_map(uintptr_t memory, uint64_t address, uint32_t bytes)
{
	uint64_t block = address + 4096;
	const uint32_t table[] = { 0, 0, 0, (uint32_t)block, (uint32_t)(block >> 32), bytes };
	memcpy((void *)memory, table, sizeof(table));
	__asm__ volatile ("dc cvac, %0; dsb osh" : : "r" (memory) : "memory");
	const struct {
		uint64_t address;
		uint32_t bytes;
		uint8_t cached;
		uint16_t line;
		uint32_t access, align;
		uint8_t min_bits, max_bits;
	} __attribute__((packed)) map = { address, sizeof(table), 1, 128, 3, 4096, 8, 64 };
	uint32_t token = VOICE_PCM_TOKEN | (++voice_pcm_serial & 0x0fffffffu);
	os_atomic_store(&voice_cvp_map_result, 0, relaxed);
	os_atomic_store(&voice_cvp_map_token, token, release);
	bool sent = hakios_adsp_apr_ports(SDX55_APR_MVM, voice_audio.source, voice_audio.mvm, token,
	    VOICE_CVP_MAP, &map, sizeof(map), 0, NULL);
	uint32_t handle = 0;
	for (unsigned elapsed = 0; sent && elapsed < 400; elapsed += 2) {
		handle = os_atomic_load(&voice_cvp_map_result, acquire);
		if (handle) break;
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	os_atomic_store(&voice_cvp_map_token, 0, release);
	hakios_tl("UMI SDX55: voice DSP map bytes=%u sent=%u reply=%u mapped=%u\n", bytes, sent, handle != 0,
	    handle != 0 && handle != UINT32_MAX);
	return handle == UINT32_MAX ? 0 : handle;
}

static bool
sdx55_voice_cvp_push(uintptr_t page, uint64_t address, uint16_t rate, uint16_t request)
{
	if (request < rate / 25) request = rate / 25;
	unsigned bytes = (request + 128u) & ~127u;
	if ((rate != 8000 && rate != 16000) || bytes > 4096) return false;
	const struct {
		uint32_t point, flags;
		uint64_t out;
		uint16_t out_bytes;
		uint64_t in;
		uint16_t in_bytes, rate, in_channels;
	} __attribute__((packed)) push = { VOICE_CVP_RX_POINT, VOICE_CVP_OUTPUT, address, (uint16_t)bytes, 0, 0, rate, 1 };
	memset((void *)page, VOICE_CVP_DMA_PATTERN & 0xff, bytes);
	for (uintptr_t line = page; line < page + bytes; line += 64)
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	__asm__ volatile ("dsb osh" ::: "memory");
	os_atomic_store(&voice_cvp_buffer, address, release);
	return hakios_adsp_apr_ports(SDX55_APR_CVP, voice_audio.source, voice_audio.cvp, VOICE_CVP_PUSH_TOKEN,
	    VOICE_CVP_PUSH, &push, sizeof(push), 0, NULL);
}

static uint32_t
sdx55_voice_cvp_play(uintptr_t page, unsigned bytes, unsigned rate, unsigned channels, bool *audible, bool *written)
{
	if ((rate != 8000 && rate != 16000) || !channels || channels > 2 || !bytes || bytes > 4096 || bytes % (2 * channels)) return 0;
	for (uintptr_t line = page; line < page + bytes; line += 64)
		__asm__ volatile ("dc ivac, %0" : : "r" (line));
	__asm__ volatile ("dsb sy" ::: "memory");
	const int16_t *input = (const int16_t *)page;
	int16_t output[480];
	unsigned ratio = 48000 / rate, frames = bytes / (2 * channels), queued = 0;
	while (frames) {
		unsigned block = MIN(frames, (unsigned)(sizeof(output) / sizeof(output[0])) / ratio);
		for (unsigned i = 0; i < block; i++, input += channels) {
			int16_t left = input[0] == VOICE_CVP_DMA_PATTERN ? 0 : input[0];
			int16_t right = channels == 2 && input[1] != VOICE_CVP_DMA_PATTERN ? input[1] : 0;
			*written |= input[0] != VOICE_CVP_DMA_PATTERN || (channels == 2 && input[1] != VOICE_CVP_DMA_PATTERN);
			int16_t sample = channels == 2 ? ((int)left + right) / 2 : left;
			*audible |= sample > 16 || sample < -16;
			for (unsigned j = 0; j < ratio; j++) output[i * ratio + j] = sample;
		}
		queued += hakios_audio_voice_play(output, block * ratio);
		frames -= block;
	}
	return queued;
}

static bool
sdx55_voice_cvp_feed(uintptr_t page, uint64_t address)
{
	uint64_t timeout, report;
	clock_interval_to_deadline(2, NSEC_PER_SEC, &timeout);
	clock_interval_to_deadline(1, NSEC_PER_SEC, &report);
	unsigned ticks = 0, completions = 0, written_periods = 0, audible_periods = 0, queued = 0;
	bool pending = false;
	while (os_atomic_load(&voice_pcm_worker.wanted, acquire)) {
		uint64_t now = mach_absolute_time();
		if (now >= report) {
			sdx55_voice_audio_observe_mailbox("during call");
			hakios_tl("UMI SDX55: voice CVP downlink ticks=%u completions=%u written=%u nonzero_periods=%u queued=%u\n",
			    ticks, completions, written_periods, audible_periods, queued);
			clock_interval_to_deadline(2, NSEC_PER_SEC, &report);
		}
		uint64_t notice = os_atomic_xchg(&voice_cvp_notice, 0, acquire);
		if (!notice) {
			if (now >= timeout) break;
			delay_for_interval(2, NSEC_PER_MSEC);
			continue;
		}
		unsigned flags = notice >> 56, channels = notice >> 48 & 0xffu;
		uint16_t rate = notice >> 32, request = notice >> 16, filled = notice;
		if (flags & VOICE_CVP_ERROR) {
			hakios_tl("UMI SDX55: voice CVP output error flags=%#x rate=%u bytes=%u request=%u\n", flags, rate, filled, request);
			break;
		}
		if (flags & VOICE_CVP_OUTPUT) {
			if (!completions)
				hakios_tl("UMI SDX55: voice CVP first output flags=%#x rate=%u channels=%u bytes=%u\n", flags, rate, channels, filled);
			bool audible = false, written = false;
			queued += sdx55_voice_cvp_play(page, filled, rate, channels, &audible, &written);
			written_periods += written;
			audible_periods += audible;
			completions++;
			pending = false;
		}
		if ((flags & VOICE_CVP_TICK) && !pending) {
			if (!ticks)
				hakios_tl("UMI SDX55: voice CVP first tick flags=%#x rate=%u request=%u\n", flags, rate, request);
			if (!sdx55_voice_cvp_push(page, address, rate, request)) break;
			ticks++;
			pending = true;
			clock_interval_to_deadline(2, NSEC_PER_SEC, &timeout);
		}
	}
	hakios_tl("UMI SDX55: voice CVP downlink stopped ticks=%u completions=%u written=%u nonzero_periods=%u queued=%u\n",
	    ticks, completions, written_periods, audible_periods, queued);
	return completions != 0;
}

static bool
sdx55_voice_cvp_pcm(void)
{
	uintptr_t memory;
	uint64_t address;
	if (!voice_audio.cvp || !hakios_audio_voice_pcm_memory(&memory, &address)) return false;
	os_atomic_store(&voice_cvp_notice, 0, relaxed);
	os_atomic_store(&voice_cvp_notifies, 0, relaxed);
	os_atomic_store(&voice_cvp_buffer, 0, release);
	os_atomic_store(&voice_cvp_port, voice_audio.cvp, release);
	hakios_adsp_apr_events_voice(sdx55_voice_cvp_event);
	uint32_t handle = sdx55_voice_map(memory, address, 4096);
	bool received = false;
	if (handle && os_atomic_load(&voice_pcm_worker.wanted, acquire)) {
		const struct {
			uint32_t handle, count, point;
			uint16_t direction, rate, duration;
		} __attribute__((packed)) tap = { handle, 1, VOICE_CVP_RX_POINT, 0, 8000, 0 };
		int error = sdx55_voice_audio_command(SDX55_APR_CVP, voice_audio.source, voice_audio.cvp, VOICE_CVP_START, &tap, sizeof(tap), NULL);
		if (!error)
			received = sdx55_voice_cvp_feed(memory + 4096, address + 4096);
		if (!error || error == ETIMEDOUT)
			(void)sdx55_voice_audio_command(SDX55_APR_CVP, voice_audio.source, voice_audio.cvp, VOICE_CVP_STOP, NULL, 0, NULL);
	}
	int unmap = handle ? sdx55_voice_audio_command(SDX55_APR_MVM, voice_audio.source, voice_audio.mvm,
	    VOICE_CVP_UNMAP, &handle, sizeof(handle), NULL) : 0;
	os_atomic_store(&voice_cvp_port, 0, release);
	hakios_audio_voice_flush();
	hakios_audio_voice_pcm_memory_put();
	return received || unmap != 0;
}
