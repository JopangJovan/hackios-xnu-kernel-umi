#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonRandom.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <CommonCrypto/CommonSymmetricKeywrap.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int lab_aes(int decrypt, const uint8_t key[32], uint8_t block[16]) {
    uint8_t tmp[16]; size_t n = 0;
    int rc = CCCrypt(decrypt ? kCCDecrypt : kCCEncrypt, kCCAlgorithmAES,
        kCCOptionECBMode, key, 32, NULL, block, 16, tmp, 16, &n);
    if (!rc && n == 16) memcpy(block, tmp, 16); else rc = -1;
    memset(tmp, 0, sizeof(tmp)); return rc;
}
static void lab_hmac(const uint8_t *k, size_t kn, const uint8_t *d, size_t dn, uint8_t out[32]) {
    CCHmac(kCCHmacAlgSHA256, k, kn, d, dn, out);
}
static int lab_random(uint8_t *out, size_t n) { return CCRandomGenerateBytes(out, n); }
#include "umi_aks_crypto.h"
int main(void) {
    uint8_t key[32], plain[64], wrapped[72], reference[72], output[64], zero[64] = {0};
    for (unsigned i = 0; i < 32; ++i) key[i] = i;
    for (unsigned i = 0; i < 64; ++i) plain[i] = i * 17;
    for (size_t n = 16; n <= 64; n += 8) {
        size_t size = sizeof(reference);
        assert(!lab_wrap(key, plain, n, wrapped));
        assert(!CCSymmetricKeyWrap(kCCWRAPAES, CCrfc3394_iv, CCrfc3394_ivLen, key, 32,
                                  plain, n, reference, &size));
        assert(size == n + 8 && !memcmp(wrapped, reference, size));
        assert(!lab_unwrap(key, wrapped, size, output) && !memcmp(output, plain, n));
        wrapped[8] ^= 1;
        assert(lab_unwrap(key, wrapped, size, output) && !memcmp(output, zero, n));
    }
    const uint8_t password[] = "public-kernel-keybag-test";
    uint8_t derived[64], expected[64];
    const uint32_t rounds[] = {1,2,7,LAB_ROUNDS};
    for (size_t i = 0; i < sizeof(rounds)/sizeof(rounds[0]); ++i) {
        assert(!lab_derive(password, sizeof(password)-1, key, rounds[i], derived));
        assert(!CCKeyDerivationPBKDF(kCCPBKDF2, (const char *)password, sizeof(password)-1,
            key, sizeof(key), kCCPRFHmacAlgSHA256, rounds[i], expected, sizeof(expected)));
        assert(!memcmp(derived, expected, sizeof(derived)));
    }
    uint8_t bag[LAB_BAG_SIZE], master[32], class1[32], class2[32];
    assert(!lab_seal_bag(password, sizeof(password)-1, plain, key, bag));
    assert(!lab_open_bag(bag, sizeof(bag), password, sizeof(password)-1, master));
    assert(!memcmp(master, key, 32));
    assert(lab_open_bag(bag, sizeof(bag), (const uint8_t *)"wrong", 5, master));
    assert(!memcmp(master, zero, 32));
    bag[64] ^= 1;
    assert(lab_open_bag(bag, sizeof(bag), password, sizeof(password)-1, master));
    assert(!memcmp(master, zero, 32));
    lab_class_key(key, plain, 4, class1); lab_class_key(key, plain, 5, class2);
    assert(!lab_equal(class1, class2, 32));
    assert(!lab_wrap(class1, plain, 64, wrapped));
    assert(lab_unwrap(class2, wrapped, 72, output) && !memcmp(output, zero, 64));
    puts("PASS: CommonCrypto KW/PBKDF2 cross-check, authenticated envelope, class separation, failure zeroing");
    return 0;
}
