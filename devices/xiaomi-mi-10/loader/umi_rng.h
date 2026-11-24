/* UMI boot entropy. Uses EFI_RNG_PROTOCOL; never falls back to a counter.
 * These checks catch a stuck/broken provider, not cryptographic certification.
 * The firmware provider's entropy source still needs a separate audit.
 */
#ifndef UMI_RNG_H
#define UMI_RNG_H
#include "efi.h"

typedef struct umi_efi_rng {
	efi_status (EFIAPI *get_info)(struct umi_efi_rng *, uint64_t *, efi_guid *);
	efi_status (EFIAPI *get_rng)(struct umi_efi_rng *, const efi_guid *, uint64_t, uint8_t *);
} umi_efi_rng;

static void
umi_rng_wipe(void *bytes, size_t size)
{
	volatile uint8_t *p = bytes;
	while (size--) *p++ = 0;
}

static efi_status
umi_rng_seed(efi_boot_services *bs, uint8_t seed[64])
{
	static const efi_guid guid = { 0x3152bca5, 0xeade, 0x433d,
	    { 0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44 } };
	static const efi_guid raw = { 0xe43176d7, 0xb6e8, 0x4827,
	    { 0xb7, 0x84, 0x7f, 0xfd, 0xc4, 0xb6, 0x85, 0x61 } };
	umi_efi_rng *rng = NULL;
	uint8_t check[64] = {0};
	efi_status status = EFI_ERROR_BIT | 3; /* unsupported */
	umi_rng_wipe(seed, 64);
	if (bs == NULL || bs->locate_protocol == NULL) goto done;
	status = bs->locate_protocol(&guid, NULL, (void **)&rng);
	if (EFI_ERROR(status)) goto done;
	if (rng == NULL || rng->get_rng == NULL) {
		status = EFI_ERROR_BIT | 3;
		goto done;
	}
	status = rng->get_rng(rng, &raw, 64, seed);
	if (EFI_ERROR(status)) goto done;
	status = rng->get_rng(rng, &raw, sizeof(check), check);
	if (EFI_ERROR(status)) goto done;
	unsigned different = 0, seed_varies = 0, check_varies = 0;
	for (size_t i = 0; i < sizeof(check); ++i) {
		different |= seed[i] ^ check[i];
		seed_varies |= seed[i] ^ seed[0];
		check_varies |= check[i] ^ check[0];
	}
	if (!different || !seed_varies || !check_varies) status = EFI_ERROR_BIT | 7;
done:
	umi_rng_wipe(check, sizeof(check));
	if (EFI_ERROR(status)) umi_rng_wipe(seed, 64);
	return status;
}
#endif
