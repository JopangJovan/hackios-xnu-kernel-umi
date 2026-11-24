/* Bounded UMI SDX55 IPv4 bearer. Only the mission worker owns MHI and
 * invokes this API. The Ethernet output callback copies packets into a queue. */
#ifndef _SYS_UMI_SDX55_NET_H_
#define _SYS_UMI_SDX55_NET_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* WDS addresses, mask and gateway in host order; zero for absent optional
 * values. The persistent en1 link goes down at the end of each bearer. */
int umi_sdx55_net_up(uint32_t address, uint32_t mask, uint32_t gateway,
    uint32_t dns1, uint32_t dns2, uint32_t mtu);
void umi_sdx55_net_down(void);
/* A capacity of 1500 is sufficient. Zero means no packet copied. */
size_t umi_sdx55_net_dequeue(void *out, size_t capacity);
/* Called after verifying and stripping the QMAP header, mux and padding. */
bool umi_sdx55_net_receive(const void *packet, size_t length);

#endif
