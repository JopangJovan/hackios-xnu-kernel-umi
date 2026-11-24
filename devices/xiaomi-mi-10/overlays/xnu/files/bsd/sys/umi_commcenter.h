#ifndef _SYS_UMI_COMMCENTER_H_
#define _SYS_UMI_COMMCENTER_H_

#include <stdbool.h>
#include <stdint.h>

/* iOS 16.5 (20F66) ApplePDPHelper selector 11, in bits per second. */
struct umi_pdp_throughput {
	uint64_t output_max;
	uint64_t input_max;
	uint64_t output_effective;
	uint64_t input_effective;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the SDX55 mission worker after DMS comes online and before teardown. */
void umi_commcenter_start(void);
void umi_commcenter_stop(void);

/* IOKit clients may update link metadata or hold the default IPv4 TX queue. */
int umi_sdx55_net_report_throughput(const struct umi_pdp_throughput *rates);
void umi_sdx55_net_hold_tx(bool hold);

#ifdef __cplusplus
}
#endif
#endif
