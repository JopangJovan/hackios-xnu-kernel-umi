/* Isolated userspace prototype. Never use with the live system keybag.
 * The caller supplies a TEST policy key independently of this directory.
 * This is not a hardware-backed retry counter or a rollback-resistant store.
 */
#ifndef UMI_SECURE_STORE_H
#define UMI_SECURE_STORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum kb_operation { KB_INITIALIZE=1, KB_AUTH_DEBIT, KB_AUTH_RESET,
                    KB_PREPARE_CHANGE, KB_COMMIT_CHANGE };
enum kb_fault_point { KB_BEFORE_WRITE=1, KB_PARTIAL_WRITE, KB_FILE_SYNCED,
                      KB_BEFORE_RENAME, KB_RENAMED, KB_DIRECTORY_SYNCED };
enum kb_item_class { KB_ITEM_WHEN_UNLOCKED=1, KB_ITEM_AFTER_FIRST_UNLOCK=2 };
enum { KB_ITEM_FORMAT=2, KB_RECORD_SIZE=256, KB_BAG_SIZE=136 };
typedef uint64_t (*kb_clock_fn)(void *);
typedef int (*kb_fault_fn)(void *, enum kb_operation, const char *, enum kb_fault_point);
struct kb_store {
    int dirfd, lockfd;
    bool loaded, poisoned, unlocked, c_ready, mirror_recovered;
    uint8_t policy[32], state[KB_RECORD_SIZE], master[32], c_root[32];
    uint64_t deadline_ms, last_ms;
    kb_clock_fn clock;
    void *clock_arg;
    kb_fault_fn fault;
    void *fault_arg;
};
int kb_open(struct kb_store *, int private_directory_fd, const uint8_t test_policy_key[32],
            kb_clock_fn, void *clock_arg);
void kb_close(struct kb_store *);
int kb_initialize(struct kb_store *, const uint8_t *, size_t);
int kb_unlock(struct kb_store *, const uint8_t *, size_t);
void kb_lock(struct kb_store *);
int kb_prepare_change(struct kb_store *, const uint8_t *old_secret, size_t,
                      const uint8_t *new_secret, size_t);
int kb_commit_change(struct kb_store *, const uint8_t *new_secret, size_t);
uint32_t kb_failures(const struct kb_store *);
uint64_t kb_epoch(const struct kb_store *);
uint64_t kb_wait_ms(struct kb_store *);
int kb_item_wrap(struct kb_store *, unsigned format, unsigned protection,
                 const uint8_t *, size_t, uint8_t *, size_t, size_t *);
int kb_item_unwrap(struct kb_store *, unsigned format, unsigned protection,
                   const uint8_t *, size_t, uint8_t *, size_t, size_t *);
#endif
