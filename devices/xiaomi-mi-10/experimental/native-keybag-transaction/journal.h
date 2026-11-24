/* Isolated diagnostic storage for opaque native keybagd property lists.
 * The injected policy key is a public fixture, not a production trust anchor. */
#ifndef UMI_NATIVE_BAG_JOURNAL_H
#define UMI_NATIVE_BAG_JOURNAL_H
#include <stddef.h>
#include <stdint.h>
enum { TJ_MAX = 16384 };
typedef int (*tj_fault_fn)(void *, const char *, unsigned);
struct tj_store { int dir, lock; uint8_t policy[32]; tj_fault_fn fault; void *arg; };
int tj_open(struct tj_store *, int directory, const uint8_t policy[32]);
void tj_close(struct tj_store *);
int tj_load(struct tj_store *, void *, size_t, size_t *, uint64_t *);
int tj_save(struct tj_store *, const void *, size_t, uint64_t expected_sequence);
enum tj_outcome { TJ_UNKNOWN, TJ_UNCHANGED, TJ_COMMITTED };
struct tj_result { enum tj_outcome outcome; uint64_t sequence; int recovered; };
/* Retry only the exact payload and expected generation. Zero means durable;
 * an error may be TJ_UNKNOWN and must not be treated as the old bag winning. */
int tj_commit(struct tj_store *, const void *, size_t, uint64_t expected_sequence,
              struct tj_result *);
#endif
