#ifndef _SYS_UMI_SDX55_VOICE_H_
#define _SYS_UMI_SDX55_VOICE_H_

#include <stdint.h>

#define UMI_VOICE_SYSCTL "kern.hakios.sdx55_voice"
#define UMI_VOICE_ROUTE_SYSCTL "kern.hakios.sdx55_voice_route"
#define UMI_VOICE_MUTE_SYSCTL "kern.hakios.sdx55_voice_mute"
#define UMI_VOICE_VERSION 1u
#define UMI_VOICE_NUMBER_SIZE 17u
#define UMI_VOICE_IMS_UNKNOWN UINT32_MAX

enum umi_voice_state {
	UMI_VOICE_UNAVAILABLE, UMI_VOICE_READY, UMI_VOICE_QUEUED, UMI_VOICE_DIALING,
	UMI_VOICE_RINGING, UMI_VOICE_ACTIVE, UMI_VOICE_ENDING, UMI_VOICE_ENDED,
	UMI_VOICE_FAILED, UMI_VOICE_UNCERTAIN
};

enum umi_voice_action {
	UMI_VOICE_DIAL = 1, UMI_VOICE_END, UMI_VOICE_USE_RECEIVER, UMI_VOICE_USE_SPEAKER,
	UMI_VOICE_MUTE, UMI_VOICE_UNMUTE
};
enum umi_voice_route { UMI_VOICE_ROUTE_NONE, UMI_VOICE_ROUTE_RECEIVER, UMI_VOICE_ROUTE_SPEAKER };

struct umi_voice_route_status {
	uint32_t route, pending, error;
};

struct umi_voice_mute_status {
	uint32_t muted, pending, error;
};

struct umi_voice_snapshot {
	uint32_t size, version, state, subscription;
	uint32_t ims_registration, ims_voice, ims_technology;
	uint32_t error, qmi_error, end_reason, call_type, call_mode;
};

struct umi_voice_request {
	uint32_t size, version, action;
	char number[UMI_VOICE_NUMBER_SIZE];
};

/* Ordinary subscriber numbers and supported carrier service numbers. */
static inline unsigned
umi_voice_number_length(const char *number)
{
	unsigned first = number[0] == '+';
	unsigned n = first;
	for (; n < UMI_VOICE_NUMBER_SIZE && number[n]; n++)
		if (number[n] < '0' || number[n] > '9') return 0;
	if (!first && n == 5 && number[0] == '1' && number[1] == '0' && number[2] == '0' &&
	    ((number[3] == '1' && number[4] == '0') || (number[3] == '8' && number[4] == '6'))) return n;
	return n < UMI_VOICE_NUMBER_SIZE && n - first >= 7 && n - first <= 15 ? n : 0;
}

#endif
