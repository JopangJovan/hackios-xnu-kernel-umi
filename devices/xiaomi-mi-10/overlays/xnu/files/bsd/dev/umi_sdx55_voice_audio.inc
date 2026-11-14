/* UMI's AUDIO_VOICE_0 is a ringless MHI channel. The ADSP's MVM
 * receives both bus addresses of the same 128 KiB mailbox. */
#define SDX55_APR_MVM 9u
#define SDX55_APR_CVS 10u
#define SDX55_APR_CVP 11u
#define SDX55_APR_RESULT 0x000110e8u
#define SDX55_VOICE_MAILBOX_CONFIG 0x0001333bu
#define SDX55_MVM_CREATE_PASSIVE 0x000110ffu
#define SDX55_CVS_CREATE_PASSIVE 0x00011140u
#define SDX55_MVM_DUAL_CONTROL 0x00011327u
#define SDX55_CVP_CREATE_V2 0x000112bfu
#define SDX55_CVP_CREATE_V3 0x00013169u
#define SDX55_CVP_CHANNELS 0x00013199u
#define SDX55_CVP_COMMIT 0x00013198u
#define SDX55_CVP_ENABLE 0x000100c6u
#define SDX55_MVM_ATTACH_CVP 0x0001123eu
#define SDX55_MVM_DETACH_CVP 0x0001123fu
#define SDX55_MVM_START 0x00011190u
#define SDX55_MVM_STOP 0x00011192u
#define SDX55_MVM_PAUSE 0x0001137du
#define SDX55_APR_DESTROY 0x0001003cu
#define SDX55_CVD_VERSION 0x00011378u
#define SDX55_CVD_VERSION_REPLY 0x00011379u
#define SDX55_CVP_SET_PARAM_V3 0x00013245u
#define SDX55_CVP_SET_PARAM_V2 0x0001133du
#define SDX55_CVP_RX_MEDIA_INFO 0x00013254u
#define SDX55_CVP_TX_MEDIA_INFO 0x00013253u
#define SDX55_CVP_EC_MEDIA_INFO 0x00013255u
#define SDX55_APR_AVCS 3u
#define SDX55_AVCS_REGISTER_TOPOLOGIES 0x00012923u
#define SDX55_AVCS_MAP 0x00012924u
#define SDX55_AVCS_MAP_REPLY 0x00012925u
#define SDX55_AVCS_UNMAP 0x00012926u
#define SDX55_VOICE_TX_TOPOLOGY 0x1000e202u
#define SDX55_VOICE_HANDSET_RX_TOPOLOGY 0x1000e20cu
#define SDX55_VOICE_SPEAKER_RX_TOPOLOGY 0x1000e200u
#define SDX55_CVP_SET_DEVICE_V3 0x0001316au

struct sdx55_voice_format {
	uint32_t port, rate;
	uint16_t channels, bits;
	uint8_t map[2];
};

static const struct sdx55_voice_format voice_handset_rx = {
	UMI_VOICE_HANDSET_RX_PORT, UMI_VOICE_HANDSET_RX_RATE, 1, 32, {3}
};
static const struct sdx55_voice_format voice_handset_tx = {
	UMI_VOICE_HANDSET_TX_PORT, UMI_VOICE_HANDSET_TX_RATE, 2, 2, {1, 2}
};
/* The modem's topologies use 2 for the 16-bit TX format. Keep the
 * speaker reference at 16 bits to share the existing playback clock. */
static const struct sdx55_voice_format voice_echo_reference = {
	UMI_VOICE_ECHO_PORT, 48000, 2, 2, {1, 0}
};

static struct umi_mhi_dma voice_mailbox;

static struct {
	uint16_t source, mvm, cvs, cvp;
	bool attempted, attached, started, microphone, speaker_echo, receiver;
	unsigned topologies;
	uint32_t cal_handle, cal_registered;
} voice_audio;

static int
sdx55_voice_audio_command(uint8_t service, uint16_t source, uint16_t destination, uint32_t opcode,
    const void *payload, unsigned length, uint16_t *handle)
{
	struct hakios_apr_answer answer;
	int error = ETIMEDOUT;
	unsigned status = 0;
	if (hakios_adsp_apr_ports(service, source, destination, 0, opcode, payload, length, 400, &answer)) {
		if (answer.opcode != SDX55_APR_RESULT || answer.length < 8 || answer.words[0] != opcode) error = EPROTO;
		else {
			status = answer.words[1];
			error = status ? EIO : 0;
			if (!error && handle) {
				*handle = answer.source_port;
				if (!*handle) error = EPROTO;
			}
		}
	}
	hakios_tl("UMI SDX55: voice APR service=%u opcode=%#x errno=%d status=%u\n", service, opcode, error, status);
	return error;
}

static int
sdx55_voice_audio_prepare(struct umi_mhi *host)
{
	static bool configured;
	if (configured) return 0;
	if (!voice_mailbox.bytes) {
		/* Both IOVAs in the stock MVM reply are aligned to the mailbox size.
		 * The DSP can round its addresses when installing this mapping. */
		int error = umi_sdx55_mhi_allocate_aligned(UMI_VOICE_MAILBOX_BYTES,
		    UMI_VOICE_MAILBOX_BYTES, &voice_mailbox);
		if (error) return error;
		bzero(voice_mailbox.bytes, voice_mailbox.size);
		umi_sdx55_mhi_sync(host->io->context, &voice_mailbox);
	}
	uint64_t dsp;
	if (!hakios_audio_voice_mailbox((uintptr_t)voice_mailbox.bytes, &dsp)) return EIO;
	struct {
		uint64_t dsp, modem;
		uint32_t size;
	} __attribute__((packed)) payload = {dsp, voice_mailbox.address, UMI_VOICE_MAILBOX_BYTES};
	int error = sdx55_voice_audio_command(SDX55_APR_MVM, 0x103, 0, SDX55_VOICE_MAILBOX_CONFIG, &payload, sizeof(payload), NULL);
	hakios_tl("UMI SDX55: voice mailbox IOVAs DSP=%#llx modem=%#llx size=%#x errno=%d\n",
	    dsp, voice_mailbox.address, UMI_VOICE_MAILBOX_BYTES, error);
	configured = !error;
	return error;
}

static int
sdx55_voice_audio_media(uint32_t param, const struct sdx55_voice_format *format)
{
	struct {
		uint32_t memory_handle;
		uint64_t memory_address;
		uint32_t payload_size, module;
		uint16_t instance, reserved;
		uint32_t parameter, parameter_size;
		struct {
			uint32_t port;
			uint16_t channels, bits;
			uint32_t rate;
			uint8_t map[32];
		} media;
	} __attribute__((packed)) payload = {
		.payload_size=60, .module=0x0001316e, .parameter=param, .parameter_size=44,
		.media={.port=format->port, .channels=format->channels, .bits=format->bits, .rate=format->rate}
	};
	memcpy(payload.media.map, format->map, sizeof(format->map));
	return sdx55_voice_audio_command(SDX55_APR_CVP, voice_audio.source, voice_audio.cvp,
	    SDX55_CVP_SET_PARAM_V3, &payload, sizeof(payload), NULL);
}

static int
sdx55_voice_cvp_param(uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	const struct {
		uint32_t handle;
		uint64_t address;
		uint32_t payload_size, module, parameter;
		uint16_t parameter_size, reserved;
	} __attribute__((packed)) header = { 0, 0, 12u + size, module, param, size, 0 };
	uint8_t payload[sizeof(header) + 72];
	if (size > sizeof(payload) - sizeof(header)) return EINVAL;
	memcpy(payload, &header, sizeof(header));
	memcpy(payload + sizeof(header), data, size);
	return sdx55_voice_audio_command(SDX55_APR_CVP, voice_audio.source, voice_audio.cvp,
	    SDX55_CVP_SET_PARAM_V2, payload, sizeof(header) + size, NULL);
}

static int
sdx55_voice_cvp_channel_info(uint32_t parameter, const struct sdx55_voice_format *format)
{
	struct { uint32_t channels, width; uint8_t map[32]; } info = {format->channels, format->bits, {0}};
	memcpy(info.map, format->map, sizeof(format->map));
	return sdx55_voice_cvp_param(0x1316e, parameter, &info, sizeof(info));
}

static bool
sdx55_voice_cvp_channels(bool echo)
{
	struct sdx55_voice_format ec_format = echo ? voice_echo_reference : voice_handset_rx;
	/* Stock stereo channel info uses FL/FR. Its endpoint media map only
	 * identifies the first reference channel. */
	if (echo) ec_format.map[1] = 2;
	int rx = sdx55_voice_cvp_channel_info(0x1328f, &voice_handset_rx);
	int tx = sdx55_voice_cvp_channel_info(0x1328e, &voice_handset_tx);
	int ec = sdx55_voice_cvp_channel_info(0x13290, &ec_format);
	hakios_tl("UMI SDX55: voice CVP channel format errno RX=%d TX=%d EC=%d\n", rx, tx, ec);
	return !rx && !tx && !ec;
}

static unsigned
sdx55_voice_handset_topologies(bool stock_tx)
{
	/* UMI's ACDB voice-handset RX and voice-dmic-ef TX definitions. The
	 * firmware also provides the instance-free TX definition, which fits
	 * in a 128-byte GLINK intent with the APR and AVCS headers. */
	static const uint32_t topologies[][15] = {
		{1, 2, SDX55_VOICE_HANDSET_RX_TOPOLOGY, 8, 5, 0x10efa, 0x10000, 0x10ee8, 0x10000, 0x1000e101, 0x10000,
		    0x10943, 0x10000, 0x10ef5, 0x10000},
		{1, 0, SDX55_VOICE_TX_TOPOLOGY, 8, 10, 0x10ee7, 0x10ee9, 0x10ef0, 0x10ef1, 0x10ef3, 0x10ef4,
		    0x10ef7, 0x10f00, 0x1000e100, 0x10f11},
	};
	static unsigned registered;
	if (stock_tx) registered |= 2u; /* The speaker pair registers the same TX topology. */
	for (unsigned i = 0; i < sizeof(topologies) / sizeof(topologies[0]); i++) {
		if (registered & (1u << i)) continue;
		struct { uint32_t low, high, handle, size, topology[15]; } packet = {.size=sizeof(topologies[0])};
		memcpy(packet.topology, topologies[i], sizeof(topologies[i]));
		if (!sdx55_voice_audio_command(SDX55_APR_AVCS, 0, 0, SDX55_AVCS_REGISTER_TOPOLOGIES,
		    &packet, sizeof(packet), NULL))
			registered |= 1u << i;
	}
	return registered;
}

struct sdx55_voice_ports {
	uint16_t tx;
	uint32_t tx_topology;
	uint16_t rx;
	uint32_t rx_topology, mixing;
	uint16_t echo_ref;
} __attribute__((packed));

static struct sdx55_voice_ports
sdx55_voice_ports(unsigned topologies, bool echo)
{
	uint32_t rx = (topologies & 1u) ? SDX55_VOICE_HANDSET_RX_TOPOLOGY : 0x10f77;
	return (struct sdx55_voice_ports){
		.tx = UMI_VOICE_HANDSET_TX_PORT,
		.tx_topology = (topologies & 2u) ? SDX55_VOICE_TX_TOPOLOGY : 0x10f70,
		.rx = UMI_VOICE_HANDSET_RX_PORT,
		.rx_topology = echo ? SDX55_VOICE_SPEAKER_RX_TOPOLOGY : rx,
		.mixing = echo ? 0x10f7d : 0x10f7c,
		.echo_ref = echo ? UMI_VOICE_ECHO_PORT : 0xffff,
	};
}

static int
sdx55_voice_cvp_format(bool modern, bool media_format, bool echo)
{
	int error = 0;
	if (modern && (!media_format || !sdx55_voice_cvp_channels(echo))) {
		if (echo) return EIO;
		const uint16_t channels[] = {voice_handset_tx.channels, voice_handset_rx.channels};
		error = sdx55_voice_audio_command(SDX55_APR_CVP, voice_audio.source, voice_audio.cvp,
		    SDX55_CVP_CHANNELS, channels, sizeof(channels), NULL);
	}
	if (!error && media_format) error = sdx55_voice_audio_media(SDX55_CVP_RX_MEDIA_INFO, &voice_handset_rx);
	if (!error && media_format) error = sdx55_voice_audio_media(SDX55_CVP_TX_MEDIA_INFO, &voice_handset_tx);
	if (!error && echo) error = sdx55_voice_audio_media(SDX55_CVP_EC_MEDIA_INFO, &voice_echo_reference);
	if (!error && modern) error = sdx55_voice_audio_command(SDX55_APR_CVP, voice_audio.source, voice_audio.cvp,
	    SDX55_CVP_COMMIT, NULL, 0, NULL);
	return error;
}

static int
sdx55_voice_cvp_configure(bool modern, bool media_format, unsigned topologies, bool echo)
{
	const struct sdx55_voice_ports ports = sdx55_voice_ports(topologies, echo);
	const struct {
		uint16_t direction, tx;
		uint32_t tx_topology;
		uint16_t rx;
		uint32_t rx_topology, profile, mixing;
		uint16_t echo_ref;
		char name[20];
	} __attribute__((packed)) devices = {
		.direction=2, .tx=ports.tx, .tx_topology=ports.tx_topology, .rx=ports.rx, .rx_topology=ports.rx_topology,
		.profile=0x1135e, .mixing=ports.mixing, .echo_ref=ports.echo_ref
	};
	int error = sdx55_voice_audio_command(SDX55_APR_CVP, voice_audio.source, 0,
	    modern ? SDX55_CVP_CREATE_V3 : SDX55_CVP_CREATE_V2, &devices, sizeof(devices), &voice_audio.cvp);
	return error ? error : sdx55_voice_cvp_format(modern, media_format, echo);
}

static void
sdx55_voice_audio_observe_mailbox(const char *when)
{
	if (!voice_mailbox.bytes) return;
	const volatile uint32_t *words = voice_mailbox.bytes;
	static uint32_t last;
	uint32_t checksum = 2166136261u;
	unsigned touched = 0, first = UMI_VOICE_MAILBOX_BYTES, last_offset = 0;
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < UMI_VOICE_MAILBOX_BYTES / sizeof(*words); i++) {
		uint32_t word = words[i];
		if (word) {
			if (!touched) first = i * sizeof(*words);
			last_offset = i * sizeof(*words);
			touched++;
		}
		checksum = (checksum ^ word) * 16777619u;
	}
	uint32_t fault = umi_sdx55_direct_read(umi_sdx55_dma.cb, 0x58) & SDX55_SMMU_FSR_FAULT;
	hakios_tl("UMI SDX55: voice mailbox %s nonzero_words=%u changed=%u offsets=%#x..%#x modem_dma_fault=%#x\n",
	    when, touched, checksum != last, first, last_offset, fault);
	last = checksum;
	hakios_adsp_voice_diag_report();
	sdx55_modem_diag_report();
}

#include "umi_sdx55_voice_pcm.inc"
#include "umi_sdx55_voice_cal.inc"

static unsigned
sdx55_voice_audio_route(void)
{
	if (!voice_audio.started) return UMI_VOICE_ROUTE_NONE;
	return voice_audio.receiver ? UMI_VOICE_ROUTE_RECEIVER : UMI_VOICE_ROUTE_SPEAKER;
}

/* Change only the ADSP devices on the active call. The CVS session, both
 * codec DMA ports and the modem call stay in place, as on Android. */
static int
sdx55_voice_audio_set_devices(bool speaker)
{
	sdx55_voice_cal_deregister();
	sdx55_voice_cal_unmap();
	voice_audio.speaker_echo = speaker;
	const struct sdx55_voice_ports ports = sdx55_voice_ports(voice_audio.topologies, speaker);
	int error = sdx55_voice_audio_command(SDX55_APR_CVP, voice_audio.source, voice_audio.cvp,
	    SDX55_CVP_SET_DEVICE_V3, &ports, sizeof(ports), NULL);
	if (!error) error = sdx55_voice_cvp_format(true, true, speaker);
	if (!error) sdx55_voice_cal_register();
	/* A route with the wrong calibration can send speaker audio back to the far end. */
	if (!error && voice_audio.cal_registered != 0xf) error = EIO;
	return error;
}

static int
sdx55_voice_audio_resume(void)
{
	return sdx55_voice_audio_command(SDX55_APR_MVM, voice_audio.source, voice_audio.mvm,
	    SDX55_MVM_START, NULL, 0, NULL);
}

static int
sdx55_voice_audio_switch(unsigned route)
{
	if (route == sdx55_voice_audio_route()) return 0;
	if (!voice_audio.started || voice_audio.topologies != 3 || (!voice_audio.receiver && !voice_audio.speaker_echo))
		return ENXIO;
	bool speaker = route == UMI_VOICE_ROUTE_SPEAKER;
	if (!speaker && !hakios_mic_voice_receiver(true)) return EIO;
	if (speaker) {
		hakios_audio_voice_speaker_hold(true);
		if (!hakios_audio_voice_echo_start()) {
			hakios_audio_voice_speaker_hold(false);
			return EIO;
		}
	}
	int error = sdx55_voice_audio_command(SDX55_APR_MVM, voice_audio.source, voice_audio.mvm,
	    SDX55_MVM_PAUSE, NULL, 0, NULL);
	if (error) goto release_prepared;
	sdx55_voice_pcm_stop();
	if (speaker && !hakios_mic_voice_receiver_mute(true)) error = EIO;
	if (!error) error = sdx55_voice_audio_set_devices(speaker);
	if (!error && !speaker && !hakios_mic_voice_receiver_mute(false)) error = EIO;
	if (!error) error = sdx55_voice_audio_resume();
	if (!error) {
		voice_audio.receiver = !speaker;
		if (speaker) {
			(void)hakios_mic_voice_receiver(false);
			sdx55_voice_audio_connected();
		} else {
			hakios_audio_voice_echo_stop();
			hakios_audio_voice_speaker_hold(false);
		}
		return 0;
	}
	if (!speaker) (void)hakios_mic_voice_receiver_mute(true);
	int restored = sdx55_voice_audio_set_devices(!speaker);
	if (speaker && !restored && !hakios_mic_voice_receiver_mute(false)) restored = EIO;
	int resumed = sdx55_voice_audio_resume();
	if (!speaker && !restored && !resumed) sdx55_voice_audio_connected();
	hakios_tl("UMI SDX55: voice route=%u errno=%d restore=%d resume=%d\n", route, error, restored, resumed);
release_prepared:
	if (speaker) {
		hakios_audio_voice_echo_stop();
		hakios_audio_voice_speaker_hold(false);
	} else {
		(void)hakios_mic_voice_receiver(false);
	}
	return error;
}

static void
sdx55_voice_audio_stop(void)
{
	sdx55_voice_pcm_stop();
	hakios_audio_voice_speaker_hold(false);
	if (voice_audio.receiver) (void)hakios_mic_voice_receiver_mute(true);
	if (voice_audio.started) {
		sdx55_voice_audio_observe_mailbox("end");
		(void)sdx55_voice_audio_command(SDX55_APR_MVM, voice_audio.source, voice_audio.mvm, SDX55_MVM_STOP, NULL, 0, NULL);
	}
	if (voice_audio.attached)
		(void)sdx55_voice_audio_command(SDX55_APR_MVM, voice_audio.source, voice_audio.mvm, SDX55_MVM_DETACH_CVP,
		    &voice_audio.cvp, sizeof(voice_audio.cvp), NULL);
	sdx55_voice_cal_deregister();
	if (voice_audio.cvp)
		(void)sdx55_voice_audio_command(SDX55_APR_CVP, voice_audio.source, voice_audio.cvp, SDX55_APR_DESTROY, NULL, 0, NULL);
	if (voice_audio.speaker_echo) hakios_audio_voice_echo_stop();
	if (voice_audio.cvs)
		(void)sdx55_voice_audio_command(SDX55_APR_CVS, voice_audio.source, voice_audio.cvs, SDX55_APR_DESTROY, NULL, 0, NULL);
	sdx55_voice_cal_unmap();
	if (voice_audio.mvm)
		(void)sdx55_voice_audio_command(SDX55_APR_MVM, voice_audio.source, voice_audio.mvm, SDX55_APR_DESTROY, NULL, 0, NULL);
	if (voice_audio.microphone) hakios_mic_voice_stop();
	hakios_adsp_voice_diag_call(false);
	bzero(&voice_audio, sizeof(voice_audio));
}

static void
sdx55_voice_audio_start(unsigned subscription)
{
	if (voice_audio.attempted || (subscription != 1 && subscription != 2)) return;
	voice_audio.attempted = true;
	hakios_adsp_voice_diag_call(true);
	/* Start warming the speaker while the modem and DSP set up the call. */
	hakios_audio_voice_speaker_hold(true);
	uint16_t source = subscription == 2 ? 7 : 6;
	voice_audio.source = source;
	voice_audio.microphone = hakios_mic_voice_start();
	hakios_tl("UMI SDX55: voice call codec path owned=%u\n", voice_audio.microphone);
	const char mmode1[20] = "11C05000", mmode2[20] = "11DC5000";
	const char *name = subscription == 2 ? mmode2 : mmode1;

	struct hakios_apr_answer version;
	bool modern = false;
	bool media_format = false;
	if (hakios_adsp_apr_ports(SDX55_APR_MVM, source, 0, 0, SDX55_CVD_VERSION, NULL, 0, 400, &version) &&
	    version.opcode == SDX55_CVD_VERSION_REPLY && version.length >= 3) {
		const char *value = (const char *)version.words;
		modern = value[0] == '2' && value[1] == '.' && value[2] >= '2' && value[2] <= '9';
		media_format = modern && value[2] >= '3';
		hakios_tl("UMI SDX55: voice CVD uses CVP v%u media_format=%d\n", modern ? 3 : 2, media_format);
	}
	int error = sdx55_voice_audio_command(SDX55_APR_MVM, source, 0, SDX55_MVM_CREATE_PASSIVE,
	    name, sizeof(mmode2), &voice_audio.mvm);
	if (!error) error = sdx55_voice_audio_command(SDX55_APR_CVS, source, 0, SDX55_CVS_CREATE_PASSIVE,
	    name, sizeof(mmode2), &voice_audio.cvs);
	const uint8_t dual = 1;
	if (!error) error = sdx55_voice_audio_command(SDX55_APR_MVM, source, voice_audio.mvm, SDX55_MVM_DUAL_CONTROL,
	    &dual, sizeof(dual), NULL);
	bool speaker_topologies = !error && media_format && sdx55_voice_register_speaker_topologies();
	unsigned topologies = !error && modern ? sdx55_voice_handset_topologies(speaker_topologies) : 0;
	voice_audio.topologies = speaker_topologies ? topologies : 0;
	hakios_tl("UMI SDX55: voice CVP topologies handset=%#x speaker=%u\n", topologies, speaker_topologies);
	if (!error) {
		voice_audio.receiver = speaker_topologies && voice_audio.microphone && hakios_mic_voice_receiver(true);
		if (voice_audio.receiver && !hakios_mic_voice_receiver_mute(false)) {
			(void)hakios_mic_voice_receiver(false);
			voice_audio.receiver = false;
		}
		if (voice_audio.receiver) hakios_audio_voice_speaker_hold(false);
		voice_audio.speaker_echo = !voice_audio.receiver && speaker_topologies && voice_audio.microphone &&
		    hakios_audio_voice_echo_start();
		error = sdx55_voice_cvp_configure(modern, media_format, topologies, voice_audio.speaker_echo);
		if (error && voice_audio.speaker_echo) {
			hakios_tl("UMI SDX55: voice speaker echo reference rejected errno=%d\n", error);
			/* Release the provisional CVP before retrying the known handset configuration. */
			if (voice_audio.cvp) {
				error = sdx55_voice_audio_command(SDX55_APR_CVP, source, voice_audio.cvp,
				    SDX55_APR_DESTROY, NULL, 0, NULL);
				if (!error) voice_audio.cvp = 0;
			} else if (error == EIO) error = 0;
			if (!error) {
				hakios_audio_voice_echo_stop();
				voice_audio.speaker_echo = false;
				error = sdx55_voice_cvp_configure(modern, media_format, topologies, false);
			}
		}
		hakios_tl("UMI SDX55: voice speaker echo reference=%u CVP errno=%d\n", voice_audio.speaker_echo, error);
	}
	if (!error && modern) sdx55_voice_cal_register();
	if (!error) error = sdx55_voice_audio_command(SDX55_APR_CVP, source, voice_audio.cvp, SDX55_CVP_ENABLE, NULL, 0, NULL);
	if (!error) {
		error = sdx55_voice_audio_command(SDX55_APR_MVM, source, voice_audio.mvm, SDX55_MVM_ATTACH_CVP,
		    &voice_audio.cvp, sizeof(voice_audio.cvp), NULL);
		voice_audio.attached = !error;
	}
	if (!error) {
		const uint32_t tty = 0;
		(void)sdx55_voice_audio_command(SDX55_APR_MVM, source, voice_audio.mvm, 0x11196, &tty, sizeof(tty), NULL);
		const struct { uint16_t direction; uint32_t step; uint16_t ramp; } __attribute__((packed)) volume = {1, 0, 20};
		(void)sdx55_voice_audio_command(SDX55_APR_CVP, source, voice_audio.cvp, 0x112c2, &volume, sizeof(volume), NULL);
		/* Android opens the call stream with the TX mute cleared. The
		 * hardware decimator stays muted until the modem reports ACTIVE. */
		const uint16_t unmute[] = {0, 0, 20};
		if (voice_audio.microphone)
			(void)sdx55_voice_audio_command(SDX55_APR_CVS, source, voice_audio.cvs, 0x1138b, unmute, sizeof(unmute), NULL);
		error = sdx55_voice_audio_command(SDX55_APR_MVM, source, voice_audio.mvm, SDX55_MVM_START, NULL, 0, NULL);
		voice_audio.started = !error;
		if (!error) {
			const uint16_t unmute_rx[] = {1, 0, 20};
			(void)sdx55_voice_audio_command(SDX55_APR_CVP, source, voice_audio.cvp, 0x112c2,
			    &volume, sizeof(volume), NULL);
			(void)sdx55_voice_audio_command(SDX55_APR_CVP, source, voice_audio.cvp, 0x1138b,
			    unmute_rx, sizeof(unmute_rx), NULL);
			sdx55_voice_audio_observe_mailbox("start");
		}
	}
	if (error) {
		sdx55_voice_audio_stop();
		voice_audio.attempted = true;
	}
	hakios_tl("UMI SDX55: voice DSP session subscription=%u errno=%d\n", subscription, error);
}
