/* Test the actual loader helper with a fault-injecting EFI protocol. */
#include <assert.h>
#include <string.h>
#include "../../loader/umi_rng.h"

static unsigned mode, calls;
static efi_status fake_rng(umi_efi_rng *self, const efi_guid *algorithm,
                          uint64_t size, uint8_t *out)
{
    assert(self != NULL && algorithm != NULL && size == 64);
    assert(algorithm->a == 0xe43176d7 && algorithm->b == 0xb6e8 && algorithm->c == 0x4827);
    const uint8_t tail[] = {0xb7,0x84,0x7f,0xfd,0xc4,0xb6,0x85,0x61};
    assert(!memcmp(algorithm->d, tail, sizeof(tail)));
    ++calls;
    for (unsigned i = 0; i < size; ++i) out[i] = (uint8_t)(i + calls);
    if ((mode == 4 && calls == 1) || (mode == 5 && calls == 2)) return EFI_ERROR_BIT | 7;
    if (mode == 6) for (unsigned i = 0; i < size; ++i) out[i] = (uint8_t)i;
    if ((mode == 7 && calls == 1) || (mode == 8 && calls == 2)) memset(out, 0, size);
    if ((mode == 9 && calls == 1) || (mode == 10 && calls == 2)) memset(out, 255, size);
    return 0;
}
static efi_status locate(const efi_guid *guid, void *registration, void **out)
{
    static umi_efi_rng rng;
    assert(guid->a == 0x3152bca5 && guid->b == 0xeade && guid->c == 0x433d);
    const uint8_t tail[] = {0x86,0x2e,0xc0,0x1c,0xdc,0x29,0x1f,0x44};
    assert(!memcmp(guid->d, tail, sizeof(tail)) && registration == NULL);
    if (mode == 1) return EFI_ERROR_BIT | 14;
    if (mode == 2) { *out = NULL; return 0; }
    rng.get_rng = mode == 3 ? NULL : fake_rng;
    *out = &rng;
    return 0;
}
int main(void)
{
    efi_boot_services bs = {0};
    bs.locate_protocol = locate;
    for (mode = 0; mode <= 10; ++mode) {
        uint8_t seed[64];
        memset(seed, 0xa5, sizeof(seed));
        calls = 0;
        efi_status rc = umi_rng_seed(&bs, seed);
        assert(EFI_ERROR(rc) == (mode != 0));
        for (unsigned i = 0; i < sizeof(seed); ++i)
            assert(seed[i] == (mode == 0 ? (uint8_t)(i + 1) : 0));
        if (mode == 0) assert(calls == 2);
    }
    uint8_t seed[64];
    memset(seed, 0xa5, sizeof(seed));
    assert(EFI_ERROR(umi_rng_seed(NULL, seed)));
    for (unsigned i = 0; i < sizeof(seed); ++i) assert(seed[i] == 0);
    bs.locate_protocol = NULL;
    assert(EFI_ERROR(umi_rng_seed(&bs, seed)));
    return 0;
}
