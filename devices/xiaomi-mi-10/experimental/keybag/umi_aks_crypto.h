/* Experimental software keybag envelope; NOT Apple's serialized keybag format.
 * Platform callbacks supply AES-256 ECB, HMAC-SHA256, and random bytes.
 * Only synthetic fixtures are authorized for the current kernel backend.
 * AES key wrap: RFC 3394. PBKDF2: RFC 8018. No hardware key binding.
 */
#ifndef UMI_AKS_CRYPTO_H
#define UMI_AKS_CRYPTO_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { LAB_BAG_SIZE = 136, LAB_BAG_MAC = 104, LAB_ROUNDS = 600000,
       LAB_MAX_SECRET = 128, LAB_OPT_IN = 0x7e, LAB_MAGIC = 0x554d494c };
static void lab_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}
static int lab_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    volatile uint8_t d = 0;
    while (n--) d |= *a++ ^ *b++;
    return d == 0;
}
static void lab_put32(uint8_t *p, uint32_t v) {
    for (int i = 3; i >= 0; --i) { p[i] = (uint8_t)v; v >>= 8; }
}
static uint32_t lab_get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void lab_xor_counter(uint8_t a[8], unsigned t) {
    for (int i = 7; i >= 0; --i) { a[i] ^= (uint8_t)t; t >>= 8; }
}
static int lab_wrap(const uint8_t kek[32], const uint8_t *in, size_t n, uint8_t *out) {
    uint8_t b[16] = {0};
    if (n < 16 || n > 64 || n % 8) return -1;
    memset(out, 0xa6, 8); memcpy(out + 8, in, n);
    for (unsigned j = 0; j < 6; ++j) {
        for (unsigned i = 1; i <= n / 8; ++i) {
            memcpy(b, out, 8); memcpy(b + 8, out + i * 8, 8);
            if (lab_aes(0, kek, b)) { lab_wipe(b, sizeof(b)); lab_wipe(out, n + 8); return -1; }
            memcpy(out, b, 8); lab_xor_counter(out, (unsigned)(n / 8) * j + i);
            memcpy(out + i * 8, b + 8, 8);
        }
    }
    lab_wipe(b, sizeof(b)); return 0;
}
static int lab_unwrap(const uint8_t kek[32], const uint8_t *in, size_t n, uint8_t *out) {
    uint8_t a[8], r[64], b[16], iv[8];
    if (n < 24 || n > 72 || n % 8) return -1;
    lab_wipe(out, n - 8);
    memcpy(a, in, 8); memcpy(r, in + 8, n - 8); memset(iv, 0xa6, 8);
    int rc = -1;
    for (int j = 5; j >= 0; --j) {
        for (unsigned i = (unsigned)(n / 8 - 1); i > 0; --i) {
            memcpy(b, a, 8); lab_xor_counter(b, (unsigned)(n / 8 - 1) * j + i);
            memcpy(b + 8, r + (i - 1) * 8, 8);
            if (lab_aes(1, kek, b)) goto done;
            memcpy(a, b, 8); memcpy(r + (i - 1) * 8, b + 8, 8);
        }
    }
    if (!lab_equal(a, iv, 8)) goto done;
    memcpy(out, r, n - 8); rc = 0;
done:
    lab_wipe(a, sizeof(a)); lab_wipe(b, sizeof(b)); lab_wipe(r, sizeof(r));
    return rc;
}
static int lab_derive(const uint8_t *pass, size_t len, const uint8_t salt[32],
                      uint32_t rounds, uint8_t out[64]) {
    uint8_t input[36], u[32], t[32], next[32];
    if (!len || len > LAB_MAX_SECRET || !rounds || rounds > LAB_ROUNDS) return -1;
    memcpy(input, salt, 32);
    for (unsigned block = 1; block <= 2; ++block) {
        lab_put32(input + 32, block);
        lab_hmac(pass, len, input, sizeof(input), u); memcpy(t, u, 32);
        for (uint32_t i = 1; i < rounds; ++i) {
            lab_hmac(pass, len, u, sizeof(u), next); memcpy(u, next, 32);
            for (unsigned k = 0; k < 32; ++k) t[k] ^= u[k];
        }
        memcpy(out + (block - 1) * 32, t, 32);
    }
    lab_wipe(input, sizeof(input)); lab_wipe(u, sizeof(u));
    lab_wipe(t, sizeof(t)); lab_wipe(next, sizeof(next)); return 0;
}
static int lab_bag_format(const uint8_t *bag, size_t len) {
    return len == LAB_BAG_SIZE && !memcmp(bag, "UMIAKS01", 8) &&
        lab_get32(bag + 8) == 1 && lab_get32(bag + 12) == LAB_ROUNDS;
}
static int lab_seal_bag(const uint8_t *pass, size_t len, const uint8_t uuid[16],
                        const uint8_t master[32], uint8_t bag[LAB_BAG_SIZE]) {
    uint8_t derived[64] = {0}; int rc = -1;
    memset(bag, 0, LAB_BAG_SIZE); memcpy(bag, "UMIAKS01", 8);
    lab_put32(bag + 8, 1); lab_put32(bag + 12, LAB_ROUNDS);
    memcpy(bag + 48, uuid, 16);
    if (lab_random(bag + 16, 32) || lab_derive(pass, len, bag + 16, LAB_ROUNDS, derived) ||
        lab_wrap(derived, master, 32, bag + 64)) goto done;
    lab_hmac(derived + 32, 32, bag, LAB_BAG_MAC, bag + LAB_BAG_MAC); rc = 0;
done:
    lab_wipe(derived, sizeof(derived)); if (rc) lab_wipe(bag, LAB_BAG_SIZE); return rc;
}
static int lab_open_bag(const uint8_t *bag, size_t size, const uint8_t *pass, size_t len,
                        uint8_t master[32]) {
    uint8_t derived[64] = {0}, tag[32] = {0}; int rc = -1;
    lab_wipe(master, 32);
    if (!lab_bag_format(bag, size) || lab_derive(pass, len, bag + 16, LAB_ROUNDS, derived)) goto done;
    lab_hmac(derived + 32, 32, bag, LAB_BAG_MAC, tag);
    if (!lab_equal(tag, bag + LAB_BAG_MAC, 32) || lab_unwrap(derived, bag + 64, 40, master)) goto done;
    rc = 0;
done:
    lab_wipe(derived, sizeof(derived)); lab_wipe(tag, sizeof(tag));
    if (rc) lab_wipe(master, 32); return rc;
}
static void lab_class_key(const uint8_t master[32], const uint8_t uuid[16],
                           uint32_t protection, uint8_t out[32]) {
    uint8_t info[36] = {0};
    memcpy(info, "UMI-AKS-class-v1", 16); memcpy(info + 16, uuid, 16);
    lab_put32(info + 32, protection); lab_hmac(master, 32, info, sizeof(info), out);
    lab_wipe(info, sizeof(info));
}
#endif
