/* Included by the paired controller, using its private request and journal helpers. */
#ifndef UMI_SECD_LIFECYCLE_TEST_H
#define UMI_SECD_LIFECYCLE_TEST_H

#include "../native-keybag-transaction/auth-notify.h"
#include "legacy-migration.h"
#include <notify.h>

/* SQLite's no-follow open also rejects the /var directory alias. Use the
 * physical directory and let that check reject all remaining symlinks. */
static const char legacy_path[] = "/private" UMI_SECD_TXN_ROOT "/legacy.sqlite";

static uint64_t wait_auth_notice(int token, enum umi_txn_auth_state expected, uint64_t after)
{
    uint64_t state = 0;
    for (unsigned i = 0; i < 50; ++i) {
        CHECK(notify_get_state(token, &state) == NOTIFY_STATUS_OK);
        if ((state & 7) == expected && (state >> 3) >= after) {
            int changed = 0;
            CHECK(notify_check(token, &changed) == NOTIFY_STATUS_OK && changed);
            printf("UMI lifecycle test: observed auth state=%u sequence=%llu\n", expected, state >> 3);
            return state;
        }
        usleep(100000);
    }
    fprintf(stderr, "UMI lifecycle test: notification mismatch state=%llu expected=%u\n", state, expected);
    CHECK(0);
    return 0;
}

static uint64_t check_journal(struct tj_store *store, uint64_t generation, unsigned failures,
                              uint8_t blob[136])
{
    struct retry_policy retry;
    CHECK(!rp_open(&retry, store, millis, NULL) && retry.loaded);
    CHECK(retry.generation == generation && retry.failures == failures);
    CHECK(!extract(retry.payload, retry.length, blob));
    uint64_t sequence = retry.sequence;
    rp_clear(&retry);
    return sequence;
}

static void exit_after_update(void *unused)
{
    (void)unused;
    puts("UMI lifecycle test: injected process exit after flushing first converted row before commit");
    _exit(74);
}

static int migration_crash_child(void)
{
    int fd = open(UMI_SECD_TXN_ROOT "/journal", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct tj_store store;
    CHECK(fd >= 0 && !tj_open(&store, fd, policy));
    uint8_t blob[136];
    check_journal(&store, 2, 0, blob);
    struct item_keys keys = {0};
    CHECK(!ik_auth(&keys, blob, pins[1], strlen(pins[1])));
    int err = legacy_fixture_migrate(legacy_path, &keys, exit_after_update, NULL);
    fprintf(stderr, "UMI lifecycle test: migration interruption was not reached, error=%d\n", err);
    ik_clear(&keys);
    wipe(blob, sizeof(blob));
    tj_close(&store);
    close(fd);
    return 1;
}

static void refuse_removal(const char *program, int index, struct tj_store *store,
                           uint64_t generation, const uint8_t before[136], uint64_t sequence)
{
    uint8_t after[136];
    CHECK(pair(program, index, -1) != 0);
    CHECK(check_journal(store, generation, 0, after) == sequence);
    CHECK(!memcmp(before, after, sizeof(after)));
    wipe(after, sizeof(after));
    puts("UMI lifecycle test: native empty-password request refused; journal and encrypted bag unchanged PASS");
}

static void lifecycle(const char *program, struct tj_store *store, int create, int recheck)
{
    char name[64];
    umi_txn_notify_name(name, UMI_SECD_TXN_ROOT, "auth-state");
    int token = -1, first = -1;
    CHECK(notify_register_check(name, &token) == NOTIFY_STATUS_OK);
    umi_txn_notify_name(name, UMI_SECD_TXN_ROOT, "first-authentication");
    CHECK(notify_register_check(name, &first) == NOTIFY_STATUS_OK);
    int initial = 0;
    CHECK(notify_check(token, &initial) == NOTIFY_STATUS_OK);
    CHECK(notify_check(first, &initial) == NOTIFY_STATUS_OK);

    uint8_t blob[136];
    struct item_keys keys = {0};
    if (create) {
        CHECK(!pair(program, -1, 0));
        uint64_t sequence = check_journal(store, 1, 0, blob);
        uint64_t first_notice = wait_auth_notice(first, UMI_TXN_FIRST_AUTHENTICATION, 1);
        CHECK((first_notice >> 3) <= sequence);
        uint64_t observed = wait_auth_notice(token, UMI_TXN_AUTHENTICATED, 1);
        CHECK((observed >> 3) <= sequence);
        CHECK(!database("create", 1, 0));
        refuse_removal(program, 0, store, 1, blob, sequence);

        CHECK(pair(program, 1, 2) != 0);
        observed = wait_auth_notice(token, UMI_TXN_LOCKED, sequence);
        sequence = check_journal(store, 1, 1, blob);
        CHECK((observed >> 3) <= sequence);
        CHECK(!pair(program, 0, 1));
        observed = wait_auth_notice(token, UMI_TXN_AUTHENTICATED, sequence + 1);
        CHECK((observed >> 3) <= check_journal(store, 2, 0, blob));
        int repeated = 0;
        CHECK(notify_check(first, &repeated) == NOTIFY_STATUS_OK && !repeated);
        /* A published success conveys no credential or key to the database. */
        CHECK(database("verify", 2, 0) != 0);
        CHECK(!database("verify", 2, 1));
        puts("UMI lifecycle test: first authentication, revocation and subsequent authentication observed; obsolete password still refused PASS");

        CHECK(!legacy_fixture_seed(legacy_path));
        CHECK(!legacy_fixture_verify(legacy_path, NULL, 0));
        CHECK(legacy_fixture_migrate(legacy_path, &keys, NULL, NULL) == EACCES);
        CHECK(ik_auth(&keys, blob, pins[0], strlen(pins[0])) == EACCES);
        CHECK(legacy_fixture_migrate(legacy_path, &keys, NULL, NULL) == EACCES);
        char *args[] = {(char *)program, "--migration-crash", NULL};
        CHECK(child_run(program, args, 45) == 74);
        CHECK(!legacy_fixture_verify(legacy_path, NULL, 0));
        CHECK(!ik_auth(&keys, blob, pins[1], strlen(pins[1])));
        CHECK(!legacy_fixture_migrate(legacy_path, &keys, NULL, NULL));
        CHECK(!legacy_fixture_verify(legacy_path, &keys, 1));
        CHECK(!legacy_fixture_migrate(legacy_path, &keys, NULL, NULL));
        puts("UMI lifecycle test: locked migration denied, uncommitted legacy rows recovered, two converted rows verified and replayed PASS");
    } else {
        /* Recheck starts from the next committed bag when an independent
         * live-system lock transition prevented the previous final comparison. */
        uint64_t generation = recheck ? 3 : 2;
        unsigned index = recheck ? 2 : 1;
        uint64_t sequence = check_journal(store, generation, 0, blob);
        refuse_removal(program, (int)index, store, generation, blob, sequence);
        CHECK(wait_auth_notice(token, UMI_TXN_LOCKED, sequence) == (sequence << 3 | UMI_TXN_LOCKED));
        CHECK(!ik_auth(&keys, blob, pins[index], strlen(pins[index])));
        CHECK(!legacy_fixture_verify(legacy_path, &keys, 1));
        ik_clear(&keys);
        CHECK(!pair(program, (int)index, (int)index + 1));
        uint64_t observed = wait_auth_notice(first, UMI_TXN_FIRST_AUTHENTICATION, sequence + 1);
        CHECK((observed >> 3) <= check_journal(store, generation + 1, 0, blob));
        observed = wait_auth_notice(token, UMI_TXN_AUTHENTICATED, sequence + 1);
        CHECK((observed >> 3) <= check_journal(store, generation + 1, 0, blob));
        CHECK(!ik_auth(&keys, blob, pins[index + 1], strlen(pins[index + 1])));
        CHECK(!legacy_fixture_verify(legacy_path, &keys, 1));
        CHECK(!database("verify", (unsigned)generation + 1, index + 1));
        puts("UMI lifecycle test: reboot loaded locked; rekey preserved migrated rows and native A/C records PASS");
    }
    ik_clear(&keys);
    wipe(blob, sizeof(blob));
    CHECK(notify_cancel(token) == NOTIFY_STATUS_OK && notify_cancel(first) == NOTIFY_STATUS_OK);
}

#endif
