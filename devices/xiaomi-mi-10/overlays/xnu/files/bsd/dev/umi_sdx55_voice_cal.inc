/* Per-vocoder calibration from the phone's own Android firmware. */
#include "umi_sdx55_voice_cal_data.inc"

#ifdef UMI_SDX55_VOICE_CAL_AVAILABLE
#define VOICE_CAL_COLUMNS 88u
#define VOICE_CAL_ALIGN 128u
#define VOICE_CAL_MAP 0x11334u
#define VOICE_CAL_MAP_REPLY 0x11336u
#define VOICE_CAL_UNMAP 0x11337u

static uint32_t voice_cal_map_token, voice_cal_map_result;

static bool
sdx55_voice_cal_map_event(uint8_t service, __unused uint16_t port, uint32_t opcode, uint32_t token,
    const uint32_t *words, uint32_t length)
{
	if (service != SDX55_APR_MVM || !token || token != os_atomic_load(&voice_cal_map_token, acquire))
		return false;
	if (opcode == VOICE_CAL_MAP_REPLY && length >= 4)
		os_atomic_store(&voice_cal_map_result, words[0] ? words[0] : UINT32_MAX, release);
	else if (opcode == SDX55_APR_RESULT && length >= 8 && words[1])
		os_atomic_store(&voice_cal_map_result, UINT32_MAX, release);
	return true;
}

static uint32_t
sdx55_voice_cal_map(uintptr_t memory, uint64_t address, uint32_t bytes)
{
	/* The first page contains the CVD map table for the calibration data. */
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
	os_atomic_store(&voice_cal_map_result, 0, relaxed);
	os_atomic_store(&voice_cal_map_token, token, release);
	hakios_adsp_apr_events_voice(sdx55_voice_cal_map_event);
	bool sent = hakios_adsp_apr_ports(SDX55_APR_MVM, voice_audio.source, voice_audio.mvm, token,
	    VOICE_CAL_MAP, &map, sizeof(map), 0, NULL);
	uint32_t handle = 0;
	for (unsigned elapsed = 0; sent && elapsed < 400; elapsed += 2) {
		handle = os_atomic_load(&voice_cal_map_result, acquire);
		if (handle) break;
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	os_atomic_store(&voice_cal_map_token, 0, release);
	hakios_adsp_apr_events_voice(NULL);
	hakios_tl("UMI SDX55: voice DSP map bytes=%u sent=%u reply=%u mapped=%u\n", bytes, sent, handle != 0,
	    handle != 0 && handle != UINT32_MAX);
	return handle == UINT32_MAX ? 0 : handle;
}

struct voice_cal_table {
	uint32_t register_op, deregister_op;
	uint8_t service;
	const uint8_t *data, *columns;
	uint32_t size, column_size;
};

static const struct voice_cal_table voice_cal_handset[] = {
	{0x1307d, 0x1307e, SDX55_APR_CVS, voice_cal_cvs, voice_cal_cvs_columns,
	    sizeof(voice_cal_cvs), sizeof(voice_cal_cvs_columns)},
	{0x11371, 0x11372, SDX55_APR_CVP, voice_cal_device, NULL, sizeof(voice_cal_device), 0},
	{0x13079, 0x1307a, SDX55_APR_CVP, voice_cal_static, voice_cal_static_columns,
	    sizeof(voice_cal_static), sizeof(voice_cal_static_columns)},
	{0x1307b, 0x1307c, SDX55_APR_CVP, voice_cal_dynamic, voice_cal_dynamic_columns,
	    sizeof(voice_cal_dynamic), sizeof(voice_cal_dynamic_columns)},
};

#ifdef UMI_SDX55_VOICE_SPEAKER_CAL_AVAILABLE
static const struct voice_cal_table voice_cal_speaker[] = {
	{0x1307d, 0x1307e, SDX55_APR_CVS, voice_cal_speaker_cvs, voice_cal_speaker_cvs_columns,
	    sizeof(voice_cal_speaker_cvs), sizeof(voice_cal_speaker_cvs_columns)},
	{0x11371, 0x11372, SDX55_APR_CVP, voice_cal_speaker_device, NULL, sizeof(voice_cal_speaker_device), 0},
	{0x13079, 0x1307a, SDX55_APR_CVP, voice_cal_speaker_static, voice_cal_speaker_static_columns,
	    sizeof(voice_cal_speaker_static), sizeof(voice_cal_speaker_static_columns)},
	{0x1307b, 0x1307c, SDX55_APR_CVP, voice_cal_speaker_dynamic, voice_cal_speaker_dynamic_columns,
	    sizeof(voice_cal_speaker_dynamic), sizeof(voice_cal_speaker_dynamic_columns)},
};

_Static_assert(sizeof(voice_cal_speaker) == sizeof(voice_cal_handset), "UMI voice calibration table count");
_Static_assert(sizeof(voice_cal_speaker_cvs) + sizeof(voice_cal_speaker_device) + sizeof(voice_cal_speaker_static) +
    sizeof(voice_cal_speaker_dynamic) + 4 * VOICE_CAL_ALIGN <= UMI_VOICE_CAL_BYTES - 4096,
    "UMI speaker voice calibration exceeds its DMA buffer");
_Static_assert(sizeof(voice_cal_speaker_cvs_columns) <= VOICE_CAL_COLUMNS &&
    sizeof(voice_cal_speaker_static_columns) <= VOICE_CAL_COLUMNS &&
    sizeof(voice_cal_speaker_dynamic_columns) <= VOICE_CAL_COLUMNS, "UMI speaker voice calibration column limit");
#endif

_Static_assert(sizeof(voice_cal_cvs) + sizeof(voice_cal_device) + sizeof(voice_cal_static) +
    sizeof(voice_cal_dynamic) + 4 * VOICE_CAL_ALIGN <= UMI_VOICE_CAL_BYTES - 4096,
    "UMI handset voice calibration exceeds its DMA buffer");
_Static_assert(sizeof(voice_cal_cvs_columns) <= VOICE_CAL_COLUMNS &&
    sizeof(voice_cal_static_columns) <= VOICE_CAL_COLUMNS && sizeof(voice_cal_dynamic_columns) <= VOICE_CAL_COLUMNS,
    "UMI voice calibration column limit");

static bool voice_cal_unmap_failed;

/* AVCS registration is synchronous. Its topology table can use the first
 * page of the call calibration buffer before the MVM uses that page. */
#ifdef UMI_SDX55_VOICE_SPEAKER_CAL_AVAILABLE
static bool
sdx55_voice_register_speaker_topologies(void)
{
	/* Stock ACDB V2 definitions. Include the 0x8000 instance IDs used by
	 * both the TX echo processing and the speaker RX calibration tables. */
	static const uint32_t definitions[] = {
		2,
		2, SDX55_VOICE_TX_TOPOLOGY, 8, 10, 0x10ee7, 0x18000, 0x10ee9, 0x18000,
		0x10ef0, 0x18000, 0x10ef1, 0x18000, 0x10ef3, 0x18000, 0x10ef4, 0x18000,
		0x10ef7, 0x18000, 0x10f00, 0x18000, 0x1000e100, 0x18000, 0x10f11, 0x18000,
		2, SDX55_VOICE_SPEAKER_RX_TOPOLOGY, 8, 5, 0x10efa, 0x18000, 0x10ee8, 0x18000, 0x1000e101, 0x18000,
		0x10943, 0x18000, 0x10ef5, 0x18000,
	};
	static bool registered;
	if (registered) return !voice_cal_unmap_failed;
	uintptr_t memory;
	uint64_t address;
	if (voice_cal_unmap_failed || !hakios_audio_voice_cal_memory(&memory, &address)) return false;
	memcpy((void *)memory, definitions, sizeof(definitions));
	for (uintptr_t line = memory; line < memory + sizeof(definitions); line += 64)
		__asm__ volatile("dc cvac, %0" :: "r" (line) : "memory");
	__asm__ volatile("dsb osh" ::: "memory");

	const struct {
		uint16_t pool, regions;
		uint32_t flags, low, high, size;
	} mapping = {3, 1, 0, (uint32_t)address, (uint32_t)(address >> 32), 4096};
	struct hakios_apr_answer reply;
	bool answered = hakios_adsp_apr_ports(SDX55_APR_AVCS, 0, 0, 0, SDX55_AVCS_MAP,
	    &mapping, sizeof(mapping), 400, &reply);
	if (!answered || reply.opcode != SDX55_AVCS_MAP_REPLY || reply.length < 4 || !reply.words[0]) {
		/* Keep the backing page pinned when the DSP may have mapped it. */
		voice_cal_unmap_failed = !answered || reply.opcode != SDX55_APR_RESULT || reply.length < 8 ||
		    reply.words[0] != SDX55_AVCS_MAP || !reply.words[1];
		if (!voice_cal_unmap_failed) hakios_audio_voice_cal_memory_put();
		hakios_tl("UMI SDX55: voice speaker topology mapping failed pinned=%u\n", voice_cal_unmap_failed);
		return false;
	}
	const uint32_t handle = reply.words[0];
	const uint32_t payload[] = {mapping.low, mapping.high, handle, sizeof(definitions)};
	int error = sdx55_voice_audio_command(SDX55_APR_AVCS, 0, 0, SDX55_AVCS_REGISTER_TOPOLOGIES,
	    payload, sizeof(payload), NULL);
	voice_cal_unmap_failed = sdx55_voice_audio_command(SDX55_APR_AVCS, 0, 0, SDX55_AVCS_UNMAP,
	    &handle, sizeof(handle), NULL) != 0;
	if (!voice_cal_unmap_failed) hakios_audio_voice_cal_memory_put();
	registered = !error;
	return registered && !voice_cal_unmap_failed;
}
#else
static bool sdx55_voice_register_speaker_topologies(void) { return false; }
#endif

static void
sdx55_voice_cal_register(void)
{
	uintptr_t memory;
	uint64_t address;
	if (voice_cal_unmap_failed || !hakios_audio_voice_cal_memory(&memory, &address)) {
		hakios_tl("UMI SDX55: voice calibration memory unavailable\n");
		return;
	}
	const struct voice_cal_table *tables = voice_cal_handset;
#ifdef UMI_SDX55_VOICE_SPEAKER_CAL_AVAILABLE
	if (voice_audio.speaker_echo) tables = voice_cal_speaker;
#endif
	unsigned offset = 4096;
	for (unsigned i = 0; i < sizeof(voice_cal_handset) / sizeof(voice_cal_handset[0]); i++) {
		memcpy((void *)(memory + offset), tables[i].data, tables[i].size);
		offset += (tables[i].size + VOICE_CAL_ALIGN - 1) & ~(VOICE_CAL_ALIGN - 1);
	}
	for (uintptr_t line = memory + 4096; line < memory + offset; line += 64)
		__asm__ volatile("dc cvac, %0" :: "r" (line) : "memory");
	__asm__ volatile("dsb osh" ::: "memory");

	voice_audio.cal_handle = sdx55_voice_cal_map(memory, address, UMI_VOICE_CAL_BYTES - 4096);
	if (!voice_audio.cal_handle) {
		hakios_audio_voice_cal_memory_put();
		return;
	}

	offset = 4096;
	for (unsigned i = 0; i < sizeof(voice_cal_handset) / sizeof(voice_cal_handset[0]); i++) {
		const struct voice_cal_table *table = &tables[i];
		struct {
			uint32_t handle, address_low, address_high, size;
			uint8_t columns[VOICE_CAL_COLUMNS];
		} payload = { .handle=voice_audio.cal_handle, .address_low=(uint32_t)(address + offset),
			.address_high=(uint32_t)((address + offset) >> 32), .size=table->size };
		unsigned bytes = sizeof(payload) - sizeof(payload.columns) + table->column_size;
		if (table->column_size) memcpy(payload.columns, table->columns, table->column_size);
		uint16_t port = table->service == SDX55_APR_CVS ? voice_audio.cvs : voice_audio.cvp;
		if (!sdx55_voice_audio_command(table->service, voice_audio.source, port, table->register_op, &payload, bytes, NULL))
			voice_audio.cal_registered |= 1u << i;
		offset += (table->size + VOICE_CAL_ALIGN - 1) & ~(VOICE_CAL_ALIGN - 1);
	}
	hakios_tl("UMI SDX55: voice %s calibration tables=%#x\n",
	    voice_audio.speaker_echo ? "speaker" : "handset", voice_audio.cal_registered);
}

static void
sdx55_voice_cal_deregister(void)
{
	for (unsigned i = sizeof(voice_cal_handset) / sizeof(voice_cal_handset[0]); i-- > 0;) {
		if (!(voice_audio.cal_registered & (1u << i))) continue;
		const struct voice_cal_table *table = &voice_cal_handset[i];
		uint16_t port = table->service == SDX55_APR_CVS ? voice_audio.cvs : voice_audio.cvp;
		(void)sdx55_voice_audio_command(table->service, voice_audio.source, port, table->deregister_op, NULL, 0, NULL);
	}
	voice_audio.cal_registered = 0;
}

static void
sdx55_voice_cal_unmap(void)
{
	if (!voice_audio.cal_handle) return;
	voice_cal_unmap_failed = sdx55_voice_audio_command(SDX55_APR_MVM, voice_audio.source, voice_audio.mvm,
	    VOICE_CAL_UNMAP, &voice_audio.cal_handle, sizeof(voice_audio.cal_handle), NULL) != 0;
	if (!voice_cal_unmap_failed) {
		voice_audio.cal_handle = 0;
		hakios_audio_voice_cal_memory_put();
	}
}
#else
static bool sdx55_voice_register_speaker_topologies(void) { return false; }
static void sdx55_voice_cal_register(void) {}
static void sdx55_voice_cal_deregister(void) {}
static void sdx55_voice_cal_unmap(void) {}
#endif
