/* UMI's earpiece is WCD9380 EAR, connected to RX0 through its RX
 * SoundWire slave. The TX master remains the codec's register bus.
 * All entries run with mic_lock held and the normal call RX0 clock up.
 */
#define UMI_RECEIVER_CODEC_ID 0x000d01170224ull
#define UMI_RX_CORE_CLK 0x30eu
#define UMI_RX_NPL_CLK 0x30fu
#define UMI_RX_NATIVE_HZ 22579200u
/* UMI DT qcom,lpi-slew-offset-tbl puts GPIO3/4/5 at 8/10/12. */
#define RECEIVER_SLEW_MASK ((3u << 8) | (3u << 10) | (3u << 12))
#define RECEIVER_SLEW_VALUE ((1u << 8) | (1u << 10) | (1u << 12))

static struct block receiver_swr = { "the RX SoundWire master", 0x03210000, 0x2000 };

struct receiver_reg {
	uint16_t address;
	uint8_t mask, value;
	uint16_t wait_us;
};

/* RX0 from the codec DMA port, Class-H DEM, 0 dB at the analog control.
 * The RX0 digital gain initially sits 12 dB below Android's 0 dB. */
static const struct receiver_reg receiver_macro[] = {
	{0x108, 0x02, 0x02},
	{0x108, 0x01, 0x01},
	{0x108, 0x02, 0x00},
	{0x440, 0x08, 0x08},
	{0x408, 0x03, 0x03},
	{0x81c, 0x20, 0x00},
	{0x410, 0x03, 0x03},
	{0x200, 0x01, 0x01},
	{0x220, 0xff, 0xc0},
	{0x21c, 0x0f, 0x00},
	{0x20c, 0x3f, 0x39},
	{0x208, 0x07, 0x00},
	{0x404, 0x48, 0x48},
	{0x414, 0xff, 0xf4},
};

/* WCD9380 RX clock, RDAC3 on RX1, then the handset's Class-H supplies
 * and PA sequence. The delays follow the codec's DAC/PA callbacks. */
static const struct receiver_reg receiver_codec[] = {
	{0x3450, 0x01, 0x00}, /* RDAC3 takes RX1. */
	{0x3408, 0x03, 0x03},
	{0x3008, 0x01, 0x01},
	{0x340d, 0x40, 0x00},
	{0x340e, 0x40, 0x00},
	{0x340f, 0x40, 0x00},
	{0x3128, 0x10, 0x10},
	{0x344e, 0x04, 0x04},
	{0x3409, 0x01, 0x01},
	{0x3414, 0x02, 0x02, 5000},
	{0x30a4, 0x04, 0x00},
	{0x3008, 0x06, 0x04},
	{0x30a8, 0xf0, 0x80},
	{0x30af, 0xe0, 0xa0, 100},
	{0x3099, 0x02, 0x02},
	{0x3098, 0xff, 0x1c},
	{0x3138, 0x20, 0x20},
	{0x30bf, 0xf0, 0xc0},
	{0x30d1, 0x0e, 0x02},
	{0x30a5, 0xe0, 0xe0},
	{0x3008, 0x40, 0x40, 100},
	{0x30af, 0xe0, 0xe0, 500},
	{0x3008, 0x08, 0x08},
	{0x3008, 0x80, 0x80, 500},
	{0x3099, 0x02, 0x00},
	{0x3098, 0xff, 0x3a, 500},
	{0x3009, 0x0c, 0x04},
	{0x3465, 0x17, 0x13},
	{0x300b, 0x80, 0x80},
	{0x300a, 0xfc, 0x90, 6000}, /* EAR PA on, 0 dB. */
};

static struct {
	bool active, master, core_clock, npl_clock;
	unsigned pins, macro, codec;
	uint32_t pin_cfg[3], pin_io[3], slew;
	uint32_t macro_saved[sizeof(receiver_macro) / sizeof(receiver_macro[0])];
	uint8_t codec_saved[sizeof(receiver_codec) / sizeof(receiver_codec[0])];
} receiver;

static bool
receiver_bus_start(void)
{
	uint8_t device = codec_wait(&receiver_swr, UMI_RECEIVER_CODEC_ID);
	if (!device) return false;
	const unsigned bank = (swr_master_read(&receiver_swr, SWRM_MCP_STATUS) & 1u) ^ 1u;
	/* Kona's WCD9380 RX map: HPH_L on slave/master DP1, Class-H voltage
	 * on DP2. They carry the analog receiver even when the headphone PAs are off.
	 * Keep each port at its stock SDW word length, offset and lane. */
	static const struct {
		uint8_t interval, offset, lane, width, packing, start, end;
	} ports[] = {
		{3, 0, 1, 1, 0xff, 0xff, 0xff},
		{31, 0, 0, 7, 0, 3, 6},
	};
	swr_write_on(&receiver_swr, 0xf, SWRS_SCP_HOST_CLK_DIV2_CTL_BANK(bank), 1, 0);
	for (unsigned i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
		const typeof(ports[0]) *p = &ports[i];
		unsigned port = i + 1;
		const struct { uint16_t reg; uint8_t value; } config[] = {
			{SWRS_DP_CHANNEL_ENABLE_BANK(i, bank), 1},
			{SWRS_DP_SAMPLE_CONTROL_1_BANK(i, bank), p->interval},
			{SWRS_DP_OFFSET_CONTROL_1_BANK(i, bank), p->offset},
			{SWRS_DP_OFFSET_CONTROL_2_BANK(i, bank), 0},
			{SWRS_DP_LANE_CONTROL_BANK(i, bank), p->lane},
			{0x103 + 0x100 * i, p->width},
			{0x126 + 0x100 * i + 0x10 * bank, (uint8_t)(p->start << 4 | p->end)},
			{0x127 + 0x100 * i + 0x10 * bank, p->packing},
		};
		for (unsigned j = 0; j < sizeof(config) / sizeof(config[0]); j++) {
			if (config[j].value == 0xff) continue;
			write_id = write_id < 14 ? write_id + 1 : 0;
			swr_write_on(&receiver_swr, device, config[j].reg, config[j].value, write_id);
		}
		unsigned hw = 0x100 * port, b = 0x40 * bank;
		if (!put(&receiver_swr, "DP port", SWRM_DP_PORT_CTRL_BANK(port, bank),
		    1u << 24 | p->offset << 8 | p->interval, 0xffffffff) ||
		    !put(&receiver_swr, "DP lane", SWRM_DP_PORT_CTRL_2_BANK(port, bank), p->lane, 0xff) ||
		    !put(&receiver_swr, "DP word length", 0x102c + hw, p->width, 0xff) ||
		    !put(&receiver_swr, "DP horizontal window", SWRM_DP_PORT_HCTRL_BANK(port, bank),
		    p->start == 0xff ? 0xf0 : p->end << 4 | p->start, 0) ||
		    (p->packing != 0xff && !put(&receiver_swr, "DP block packing", 0x1038 + hw + b, p->packing, 0))) return false;
	}
	/* 50 rows × 16 columns, at 9.6 MHz with a 4 kHz frame clock. */
	if (!put_bits(&receiver_swr, "active frame shape", SWRM_MCP_FRAME_CTRL_BANK(bank), 0xff07ff, 0x5000f))
		return false;
	swr_write_on(&receiver_swr, 0xf, SWRS_SCP_FRAME_CTRL_BANK(bank), 0x0f, 0xf);
	sleep_ms(10);
	uint8_t pcm = 0, clsh = 0;
	bool ok = (swr_master_read(&receiver_swr, SWRM_MCP_STATUS) & 1u) == bank &&
	    swr_read_on(&receiver_swr, device, SWRS_DP_CHANNEL_ENABLE_BANK(0, bank), &pcm) &&
	    swr_read_on(&receiver_swr, device, SWRS_DP_CHANNEL_ENABLE_BANK(1, bank), &clsh) && pcm == 1 && clsh == 1;
	say("mic: earpiece SoundWire bank=%u PCM=%#x Class-H=%#x ready=%u\n", bank, pcm, clsh, ok);
	return ok;
}

static bool
receiver_stop(void)
{
	bool previous_fault = broken;
	bool muted = true;
	if (receiver.codec || receiver.active) {
		broken = false;
		muted = put_bits(&voice_rx_macro, "mute earpiece RX0", 0x400, 0x10, 0x10);
	}
	while (receiver.codec) {
		broken = false;
		unsigned i = --receiver.codec;
		const struct receiver_reg *reg = &receiver_codec[i];
		bool ok = codec_bits(reg->address, reg->mask, receiver.codec_saved[i]);
		if (reg->address == 0x300a) {
			muted |= ok;
			sleep_ms(7);
		} else if (reg->wait_us) spin_us(reg->wait_us);
	}
	if (receiver.master) {
		broken = false;
		(void)put(&receiver_swr, "disable RX master", SWRM_COMP_CFG, 0, 0);
		(void)put(&receiver_swr, "RX master reset", SWRM_COMP_SW_RESET, 1, 0);
		receiver.master = false;
	}
	while (receiver.macro) {
		broken = false;
		unsigned i = --receiver.macro;
		const struct receiver_reg *reg = &receiver_macro[i];
		(void)put_bits(&voice_rx_macro, "restore RX", reg->address, reg->mask, receiver.macro_saved[i]);
	}
	if (receiver.pins) {
		broken = false;
		for (unsigned i = 0; i < receiver.pins; i++) {
			(void)put_bits(&lpi, "restore RX pin", 0x1000 * (i + 3), 0x3ff, receiver.pin_cfg[i]);
			broken = false;
			(void)put_bits(&lpi, "restore RX pin direction", 0x1000 * (i + 3) + 4, 2, receiver.pin_io[i]);
			broken = false;
		}
		(void)put_bits(&lpi_slew, "restore RX pin slew", 0, RECEIVER_SLEW_MASK, receiver.slew);
		receiver.pins = 0;
	}
	if (receiver.npl_clock)
		(void)afe_clock_at("RX NPL clock off", UMI_RX_NPL_CLK, UMI_RX_NATIVE_HZ, false);
	if (receiver.core_clock)
		(void)afe_clock_at("RX core clock off", UMI_RX_CORE_CLK, UMI_RX_NATIVE_HZ, false);
	receiver.npl_clock = receiver.core_clock = receiver.active = false;
	broken = previous_fault;
	return muted;
}

static bool
receiver_start(void)
{
	if (broken) return false;
	/* Keep the TX clock votes that already feed Bolero's RX0. Also vote
	 * the RX clocks before touching the separate SoundWire controller. */
	receiver.core_clock = afe_clock_at("RX core clock on", UMI_RX_CORE_CLK, UMI_RX_NATIVE_HZ, true);
	if (!receiver.core_clock) goto fail;
	receiver.npl_clock = afe_clock_at("RX NPL clock on", UMI_RX_NPL_CLK, UMI_RX_NATIVE_HZ, true);
	if (!receiver.npl_clock) goto fail;
	receiver.slew = get(&lpi_slew, "RX pin slew", 0);
	for (unsigned i = 0; i < 3; i++) {
		unsigned pin = i + 3;
		receiver.pin_cfg[i] = get(&lpi, "RX pin mux", 0x1000 * pin);
		receiver.pin_io[i] = get(&lpi, "RX pin direction", 0x1000 * pin + 4);
		receiver.pins++;
		if (!put_bits(&lpi, "RX SoundWire pin", 0x1000 * pin, 0x3ff, i == 0 ? 4 : i == 1 ? 6 : 10) ||
		    !put_bits(&lpi, "RX SoundWire pin direction", 0x1000 * pin + 4, 2, 0)) goto fail;
	}
	if (!put_bits(&lpi_slew, "RX pin slew active", 0, RECEIVER_SLEW_MASK, RECEIVER_SLEW_VALUE)) goto fail;
	for (unsigned i = 0; i < sizeof(receiver_macro) / sizeof(receiver_macro[0]); i++) {
		const struct receiver_reg *reg = &receiver_macro[i];
		receiver.macro_saved[i] = get(&voice_rx_macro, "save RX register", reg->address);
		receiver.macro++;
		if (!put_bits(&voice_rx_macro, "configure earpiece RX", reg->address, reg->mask, reg->value)) goto fail;
	}
	receiver.master = true;
	if (!swr_master_init(&receiver_swr) || !receiver_bus_start()) goto fail;
	for (unsigned i = 0; i < sizeof(receiver_codec) / sizeof(receiver_codec[0]); i++) {
		const struct receiver_reg *reg = &receiver_codec[i];
		if (!swr_read(codec_device, reg->address, &receiver.codec_saved[i])) goto fail;
		receiver.codec++;
		if (!codec_bits(reg->address, reg->mask, reg->value)) goto fail;
		if (reg->wait_us) spin_us(reg->wait_us);
	}
	receiver.active = true;
	say("mic: earpiece prepared on WCD9380 EAR\n");
	return true;
fail:
	say("mic: earpiece startup failed after %s\n", last_write);
	(void)receiver_stop();
	broken = false;
	return false;
}
