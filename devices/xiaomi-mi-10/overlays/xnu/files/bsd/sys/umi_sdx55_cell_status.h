/* Read-only observations of the modem clients in the current UMI worker session.
 * A valid bit means the corresponding query returned decodable data in this
 * generation. NAS/WDS subscription 0 means the client's default line.
 * Masks use bit 0 for logical slot 0. No SIM or network identifiers are exported. */
#ifndef _SYS_UMI_SDX55_CELL_STATUS_H_
#define _SYS_UMI_SDX55_CELL_STATUS_H_
#include <stdint.h>
#define UMI_SDX55_CELL_SYSCTL "kern.hakios.sdx55_cell_status"
#define UMI_SDX55_CELL_VERSION 1u
#define UMI_CELL_UIM 1u
#define UMI_CELL_NAS 2u
#define UMI_CELL_NR  4u
#define UMI_CELL_WDS 8u
struct umi_sdx55_cell_snapshot {
	uint32_t size, version, generation, active, valid;
	uint32_t slots, present_mask, ready_mask, error_mask;
	uint32_t registration, ps_attach, radio_mask, data_mask;
	uint32_t nr_service, nr_domain; /* domain 255 when the NR domain TLV is absent */
	uint32_t wds_status, nas_subscription, wds_subscription;
};

#define UMI_SDX55_RADIO_SYSCTL "kern.hakios.sdx55_radio_status"
#define UMI_SDX55_DATA_SYSCTL "kern.hakios.sdx55_mobile_data"
#define UMI_SDX55_RADIO_VERSION 1u
enum umi_sdx55_data_state {
	UMI_DATA_UNAVAILABLE, UMI_DATA_OFF, UMI_DATA_CONNECTING, UMI_DATA_ON, UMI_DATA_DISCONNECTING
};
struct umi_sdx55_radio_snapshot {
	uint32_t size, version, enabled, state;
	uint32_t rat; /* 4 or 5 when registered; 0 while searching. */
	int32_t rsrp; /* dBm, INT32_MIN when unavailable. */
	char plmn[7];
	char operator_name[64]; /* Serving network name as UTF-8, when available. */
};
#endif
