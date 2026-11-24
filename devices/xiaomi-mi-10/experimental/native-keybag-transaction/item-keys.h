/* Isolated ordinary-keychain key wrapping. Not enabled in securityd. */
#ifndef UMI_ITEM_KEYS_H
#define UMI_ITEM_KEYS_H
#include <stddef.h>
#include <stdint.h>
struct item_keys {
    uint8_t a[32], c[32], uuid[16];
    int a_ready, c_ready;
};
void ik_clear(struct item_keys *s);
void ik_lock(struct item_keys *s);
int ik_auth(struct item_keys *s, const uint8_t bag[136], const void *secret, size_t n);
int ik_wrap(struct item_keys *s, unsigned cls, const void *in, size_t n, void *out, size_t *capacity);
int ik_unwrap(struct item_keys *s, unsigned cls, const void *in, size_t n, void *out, size_t *capacity);
/* Explicit synthetic-fixture conversion only: HKwr has no authentication. */
int ik_import_fixture(struct item_keys *s, unsigned cls, const void *old, size_t n, void *out, size_t *capacity);
#endif
