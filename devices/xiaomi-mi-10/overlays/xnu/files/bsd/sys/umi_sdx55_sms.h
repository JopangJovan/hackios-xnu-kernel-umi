#ifndef _SYS_UMI_SDX55_SMS_H_
#define _SYS_UMI_SDX55_SMS_H_

#include <stdint.h>

#define UMI_SMS_SYSCTL "kern.hakios.sdx55_sms"
#define UMI_SMS_SEND_SYSCTL "kern.hakios.sdx55_sms_send"
#define UMI_SMS_VERSION 1u
#define UMI_SMS_PDU_MAX 255u
#define UMI_SMS_SUBMIT_MAX 158u

struct umi_sms_snapshot {
	uint32_t version;
	uint32_t ready;
	uint64_t sequence;
	uint16_t length;
	uint8_t subscription;
	uint8_t pdu[UMI_SMS_PDU_MAX];
};

/* A zero sequence renews the receiver's lease. Completion applies only to
 * the current head. A positive network ACK follows acceptance by userland. */
struct umi_sms_receipt {
	uint64_t sequence;
	uint32_t version;
	uint32_t accepted;
};

struct umi_sms_submission {
	uint64_t token;
	uint32_t version;
	uint16_t length;
	uint8_t pdu[UMI_SMS_SUBMIT_MAX];
};

struct umi_sms_send_status {
	uint64_t token;
	uint32_t version;
	uint32_t pending;
	int32_t error;
};

#endif
