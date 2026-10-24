/* UMI PCIe0 endpoint enumeration. The platform owns mappings and RPMh votes. */
#ifndef _SYS_UMI_PCIE_H_
#define _SYS_UMI_PCIE_H_
#include <stdbool.h>
#include <stdint.h>

struct umi_pcie_io {
	void *context;
	uint32_t (*read32)(void *, uint32_t physical_address);
	void (*write32)(void *, uint32_t physical_address, uint32_t value);
	int (*power_on)(void *); /* Board-specific rail votes; no endpoint DMA. */
	void (*delay_us)(void *, unsigned);
	void (*report)(void *, const char *, uint32_t);
};
struct umi_pcie {
	const struct umi_pcie_io *io;
	bool failed, enumerated;
};
/* Explicit diagnostic only, on a zero-initialized host. Bus mastering stays
 * disabled. No SMMU, MSI or MHI context is installed by this component.
 */
int umi_pcie_enumerate(struct umi_pcie *, const struct umi_pcie_io *);
/* Hold endpoint in reset before returning to loader. Rail/clock votes remain
 * until hardware reset; shared supplies are never disabled here. */
void umi_pcie_quiesce(struct umi_pcie *);
#endif
