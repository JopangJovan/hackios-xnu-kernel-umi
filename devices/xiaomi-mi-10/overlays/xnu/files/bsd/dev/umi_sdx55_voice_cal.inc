/* Per-vocoder calibration for UMI's stock handset pair (TX 41, RX 7).
 * The calibration is supplied from the phone's firmware at build time.
 */
#include "umi_sdx55_voice_cal_data.inc"

#ifdef UMI_SDX55_VOICE_CAL_AVAILABLE
#define VOICE_CAL_COLUMNS 88u
#define VOICE_CAL_ALIGN 128u

static const struct {
	uint32_t register_op, deregister_op;
	uint8_t service;
	const uint8_t *data, *columns;
	uint32_t size, column_size;
} voice_cal_tables[] = {
	{0x1307d, 0x1307e, SDX55_APR_CVS, voice_cal_cvs, voice_cal_cvs_columns,
	    sizeof(voice_cal_cvs), sizeof(voice_cal_cvs_columns)},
	{0x11371, 0x11372, SDX55_APR_CVP, voice_cal_device, NULL, sizeof(voice_cal_device), 0},
	{0x13079, 0x1307a, SDX55_APR_CVP, voice_cal_static, voice_cal_static_columns,
	    sizeof(voice_cal_static), sizeof(voice_cal_static_columns)},
	{0x1307b, 0x1307c, SDX55_APR_CVP, voice_cal_dynamic, voice_cal_dynamic_columns,
	    sizeof(voice_cal_dynamic), sizeof(voice_cal_dynamic_columns)},
};

_Static_assert(sizeof(voice_cal_cvs) + sizeof(voice_cal_device) + sizeof(voice_cal_static) +
    sizeof(voice_cal_dynamic) + 4 * VOICE_CAL_ALIGN <= UMI_VOICE_CAL_BYTES - 4096,
    "UMI handset voice calibration exceeds its DMA buffer");
_Static_assert(sizeof(voice_cal_cvs_columns) <= VOICE_CAL_COLUMNS &&
    sizeof(voice_cal_static_columns) <= VOICE_CAL_COLUMNS && sizeof(voice_cal_dynamic_columns) <= VOICE_CAL_COLUMNS,
    "UMI voice calibration column limit");

static bool voice_cal_unmap_failed;

static void
sdx55_voice_cal_register(void)
{
	uintptr_t memory;
	uint64_t address;
	if (voice_cal_unmap_failed || !hakios_audio_voice_cal_memory(&memory, &address)) {
		hakios_tl("UMI SDX55: voice handset calibration memory unavailable\n");
		return;
	}
	unsigned offset = 4096;
	for (unsigned i = 0; i < sizeof(voice_cal_tables) / sizeof(voice_cal_tables[0]); i++) {
		memcpy((void *)(memory + offset), voice_cal_tables[i].data, voice_cal_tables[i].size);
		offset += (voice_cal_tables[i].size + VOICE_CAL_ALIGN - 1) & ~(VOICE_CAL_ALIGN - 1);
	}
	for (uintptr_t line = memory + 4096; line < memory + offset; line += 64)
		__asm__ volatile("dc cvac, %0" :: "r" (line) : "memory");
	__asm__ volatile("dsb osh" ::: "memory");

	hakios_adsp_apr_events_voice(sdx55_voice_cvp_event);
	voice_audio.cal_handle = sdx55_voice_map(memory, address, UMI_VOICE_CAL_BYTES - 4096);
	hakios_adsp_apr_events_voice(NULL);
	if (!voice_audio.cal_handle) {
		hakios_audio_voice_cal_memory_put();
		return;
	}

	offset = 4096;
	for (unsigned i = 0; i < sizeof(voice_cal_tables) / sizeof(voice_cal_tables[0]); i++) {
		const typeof(voice_cal_tables[0]) *table = &voice_cal_tables[i];
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
	hakios_tl("UMI SDX55: voice handset calibration tables=%#x\n", voice_audio.cal_registered);
}

static void
sdx55_voice_cal_deregister(void)
{
	for (unsigned i = sizeof(voice_cal_tables) / sizeof(voice_cal_tables[0]); i-- > 0;) {
		if (!(voice_audio.cal_registered & (1u << i))) continue;
		const typeof(voice_cal_tables[0]) *table = &voice_cal_tables[i];
		uint16_t port = table->service == SDX55_APR_CVS ? voice_audio.cvs : voice_audio.cvp;
		(void)sdx55_voice_audio_command(table->service, voice_audio.source, port, table->deregister_op, NULL, 0, NULL);
	}
}

static void
sdx55_voice_cal_unmap(void)
{
	if (!voice_audio.cal_handle) return;
	voice_cal_unmap_failed = sdx55_voice_audio_command(SDX55_APR_MVM, voice_audio.source, voice_audio.mvm,
	    VOICE_CVP_UNMAP, &voice_audio.cal_handle, sizeof(voice_audio.cal_handle), NULL) != 0;
	if (!voice_cal_unmap_failed) hakios_audio_voice_cal_memory_put();
}
#else
static void sdx55_voice_cal_register(void) {}
static void sdx55_voice_cal_deregister(void) {}
static void sdx55_voice_cal_unmap(void) {}
#endif
