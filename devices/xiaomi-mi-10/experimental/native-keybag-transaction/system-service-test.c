/* Original service name, native kernel system bag, and public fixture data.
 * Refuse to open a keybag on the persistent user-data mount. */
#define UMI_NATIVE_TEST_MAIN old_transaction_main
#include "native-test.c"
#undef UMI_NATIVE_TEST_MAIN
#include "retry-policy.h"
#include "item-keys.h"
#include "system-fixture.h"
#include <signal.h>
#include <time.h>
#include "fixture-volume.h"

static uint64_t milliseconds(void *unused)
{
    (void)unused;
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void fault(int directory, const char *name, int enabled)
{
    if (enabled) {
        int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        CHECK(fd >= 0 && !fsync(fd) && !close(fd));
    } else {
        CHECK(!unlinkat(directory, name, 0));
    }
    CHECK(!fsync(directory));
}

static uint64_t snapshot(struct tj_store *store, uint64_t generation, unsigned failures,
                         uint8_t blob[136])
{
    struct retry_policy retry;
    CHECK(!rp_open(&retry, store, milliseconds, NULL) && retry.loaded);
    if (retry.generation != generation || retry.failures != failures)
        printf("UMI shared retry: state mismatch generation=%llu failures=%u expected=%llu/%u\n",
               (unsigned long long)retry.generation, retry.failures,
               (unsigned long long)generation, failures);
    CHECK(retry.generation == generation && retry.failures == failures);
    CHECK(!extract(retry.payload, retry.length, blob));
    uint64_t sequence = retry.sequence;
    rp_clear(&retry);
    return sequence;
}

static unsigned request_change(const char *program, int old_pin, int new_pin)
{
    char old[8], next[8];
    snprintf(old, sizeof(old), "%d", old_pin);
    snprintf(next, sizeof(next), "%d", new_pin);
    char *args[] = {(char *)program, "--pair", old, next, NULL};
    pid_t pid;
    CHECK(!posix_spawn(&pid, program, NULL, NULL, args, environ));
    for (unsigned i = 0; i < 700; ++i) {
        int status;
        pid_t result = waitpid(pid, &status, WNOHANG);
        CHECK(result >= 0);
        if (result == pid) {
            CHECK(WIFEXITED(status));
            return (unsigned)WEXITSTATUS(status);
        }
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    CHECK(0);
    return 1;
}

static void expect_active(const uint8_t blob[136])
{
    uint8_t active[136];
    CHECK(!save(0, active) && !memcmp(active, blob, sizeof(active)));
    wipe(active, sizeof(active));
}

static const uint8_t fixture_key[32] = "PUBLIC-PROTECTED-ITEM-KEY";

static void check_protected(struct item_keys *keys, const uint8_t blob[136], int pin,
                            const uint8_t wrapped[48])
{
    uint8_t result[32];
    size_t size = sizeof(result);
    CHECK(!ik_auth(keys, blob, pins[pin], strlen(pins[pin])));
    CHECK(!ik_unwrap(keys, 6, wrapped, 48, result, &size));
    CHECK(size == sizeof(fixture_key) && !memcmp(result, fixture_key, size));
    wipe(result, sizeof(result));
    ik_clear(keys);
}

static kern_return_t authenticate(uint32_t selector, int pin)
{
    uint64_t inputs[] = {0, (uintptr_t)pins[pin], strlen(pins[pin])};
    return call(selector, inputs, 3, NULL, 0, NULL, NULL, NULL);
}

static void wrong_mixed(const char *program, struct tj_store *store, uint64_t generation,
                        unsigned limit, uint8_t blob[136])
{
    for (unsigned count = 1; count <= limit; ++count) {
        if (count % 2) {
            CHECK(authenticate(count % 4 == 1 ? 9 : 0xc, 3) == kIOReturnNotPermitted);
        } else {
            CHECK(request_change(program, 3, 2) != 0);
        }
        snapshot(store, generation, count, blob);
        if (count < limit) usleep(rp_delay_for(count) * 1000u);
    }
}

/* A delayed XPC response must not turn an expired window into a policy failure. */
static void check_wait_window(uint64_t started, unsigned failures)
{
    uint64_t elapsed = milliseconds(NULL) - started;
    uint64_t window = rp_delay_for(failures);
    printf("UMI shared retry: wait assertion elapsed=%llu window=%llu ms\n",
           (unsigned long long)elapsed, (unsigned long long)window);
    if (elapsed >= window)
        puts("UMI shared retry: timing window expired; waiting-policy result is inconclusive");
    CHECK(elapsed < window);
}

static int persistent(const char *program, int mode, int resume, int rearm)
{
    CHECK(!mount_fixture_volume(mode));
    int directory = open(UMI_SYSTEM_FIXTURE_ROOT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(directory >= 0);
    if (mode == 1) {
        CHECK(!mkdirat(directory, "keybags", 0700) && !mkdirat(directory, "journal", 0700));
        CHECK(!fsync(directory));
    }
    int journal = openat(directory, "journal", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct tj_store store;
    CHECK(journal >= 0 && !tj_open(&store, journal, policy) && !open_connection(&connection));
    uint64_t zero = 0, flags = ~0ull;
    uint8_t bag[136], saved[136], wrapped[48];
    struct item_keys keys = {0};

    if (mode == 1 || resume) {
        int old_pin = resume ? 1 : 0;
        int new_pin = old_pin + 1;
        uint64_t generation = resume ? 2 : 1;
        if (!resume) {
            uint64_t enable_system[] = {0x554d4e41, 1};
            CHECK(!call(0x7c, enable_system, 2, NULL, 0, NULL, NULL, NULL));
            CHECK(!request_change(program, -1, old_pin));
        }
        snapshot(&store, generation, 0, bag);
        size_t size = sizeof(wrapped);
        CHECK(!ik_auth(&keys, bag, pins[old_pin], strlen(pins[old_pin])));
        CHECK(!ik_wrap(&keys, 6, fixture_key, sizeof(fixture_key), wrapped, &size) && size == sizeof(wrapped));
        ik_clear(&keys);
        CHECK(!request_change(program, old_pin, new_pin));
        ++generation;
        snapshot(&store, generation, 0, bag);
        expect_active(bag);
        check_protected(&keys, bag, new_pin, wrapped);
        CHECK(!call(8, &zero, 1, NULL, 0, NULL, NULL, NULL));
        wrong_mixed(program, &store, generation, 8, bag);
        uint64_t wait_started = milliseconds(NULL);
        uint64_t sequence = snapshot(&store, generation, 8, bag);
        kern_return_t locked_result = authenticate(9, new_pin);
        unsigned change_result = request_change(program, new_pin, 4);
        check_wait_window(wait_started, 8);
        CHECK(locked_result == kIOReturnNotReady && change_result != 0);
        CHECK(snapshot(&store, generation, 8, saved) == sequence && !memcmp(bag, saved, sizeof(bag)));
        puts("UMI shared retry: lock-screen and canonical change share counter and wait PASS");
        uint8_t test_config[] = {(uint8_t)generation, (uint8_t)old_pin, (uint8_t)new_pin};
        CHECK(!fileio(directory, "restart-test-config", test_config, sizeof(test_config), 1));
        CHECK(!fileio(directory, "restart-envelope", bag, sizeof(bag), 1));
        CHECK(!fileio(directory, "restart-wrapped-key", wrapped, sizeof(wrapped), 1));
        CHECK(!fsync(directory));
    } else {
        uint8_t test_config[3];
        CHECK(!fileio(directory, "restart-test-config", test_config, sizeof(test_config), 0));
        CHECK((test_config[0] == 2 || test_config[0] == 3) &&
              test_config[1] == test_config[0] - 2 && test_config[2] == test_config[1] + 1);
        unsigned generation = test_config[0];
        int old_pin = test_config[1], new_pin = test_config[2];
        if (rearm) {
            /* Preserve the qualified bag even if a previous diagnostic left
             * a bounded failure count. Clear it only by real authentication. */
            struct retry_policy current;
            CHECK(!rp_open(&current, &store, milliseconds, NULL) && current.loaded);
            printf("UMI shared retry: rearm generation=%llu failures=%u\n",
                   (unsigned long long)current.generation, current.failures);
            CHECK(current.generation == generation && current.failures <= 8);
            CHECK(!extract(current.payload, current.length, bag));
            rp_clear(&current);
            CHECK(!fileio(directory, "restart-envelope", saved, sizeof(saved), 0) &&
                  !memcmp(bag, saved, sizeof(bag)));
            CHECK(!fileio(directory, "restart-wrapped-key", wrapped, sizeof(wrapped), 0));
            CHECK(request_change(program, 3, 2) != 0);
            /* Starting the daemon restores the full persisted backoff. */
            sleep(rp_delay_for(8) / 1000u + 1);
            CHECK(!authenticate(9, new_pin));
            snapshot(&store, generation, 0, saved);
            CHECK(!memcmp(bag, saved, sizeof(bag)));
            check_protected(&keys, bag, new_pin, wrapped);
            CHECK(!call(8, &zero, 1, NULL, 0, NULL, NULL, NULL));
            wrong_mixed(program, &store, generation, 8, saved);
            CHECK(!memcmp(bag, saved, sizeof(bag)));
            CHECK(!fsync(journal) && !fsync(directory));
            tj_close(&store);
            close(journal); close(directory); IOServiceClose(connection);
            wipe(bag, sizeof(bag)); wipe(saved, sizeof(saved)); wipe(wrapped, sizeof(wrapped));
            puts("SHARED RETRY REARM ALL PASS");
            return 0;
        }
        uint64_t sequence = snapshot(&store, generation, 8, bag);
        CHECK(!fileio(directory, "restart-envelope", saved, sizeof(saved), 0) && !memcmp(bag, saved, sizeof(bag)));
        CHECK(!fileio(directory, "restart-wrapped-key", wrapped, sizeof(wrapped), 0));
        CHECK(!call(7, &zero, 1, NULL, 0, &flags, NULL, NULL) && flags == 1);
        /* Wake the real service through XPC. The recovered attempt must wait. */
        uint64_t wait_started = milliseconds(NULL);
        CHECK(request_change(program, new_pin, 4) != 0);
        expect_active(bag);
        kern_return_t locked_result = authenticate(9, new_pin);
        check_wait_window(wait_started, 8);
        CHECK(locked_result == kIOReturnNotReady);
        CHECK(snapshot(&store, generation, 8, saved) == sequence && !memcmp(bag, saved, sizeof(bag)));
        CHECK(!call(7, &zero, 1, NULL, 0, &flags, NULL, NULL) && flags == 1);
        sleep(rp_delay_for(8) / 1000u + 1);
        CHECK(!authenticate(9, new_pin));
        CHECK(!call(7, &zero, 1, NULL, 0, &flags, NULL, NULL) && flags == 4);
        snapshot(&store, generation, 0, bag);
        check_protected(&keys, bag, new_pin, wrapped);
        CHECK(!call(8, &zero, 1, NULL, 0, NULL, NULL, NULL));
        CHECK(authenticate(9, old_pin) == kIOReturnNotPermitted);
        snapshot(&store, generation, 1, bag);
        CHECK(!authenticate(0xc, new_pin));
        snapshot(&store, generation, 0, bag);
        puts("UMI shared retry: restored bag and wait after stock reboot; correct unlock and original protected key PASS");
    }
    CHECK(!fsync(journal) && !fsync(directory));
    tj_close(&store);
    close(journal);
    close(directory);
    IOServiceClose(connection);
    wipe(bag, sizeof(bag));
    wipe(saved, sizeof(saved));
    wipe(wrapped, sizeof(wrapped));
    puts(mode == 1 || resume ? "SHARED RETRY CREATE ALL PASS" : "SHARED RETRY RESTART ALL PASS");
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    umask(077);
    CHECK(geteuid() == 0 && umi_system_fixture_volatile());
    if (argc == 4 && !strcmp(argv[1], "--pair")) {
        extern int native_request_pair(int, int);
        return native_request_pair(atoi(argv[2]), atoi(argv[3]));
    }
    int retry_mode = umi_system_fixture_retry_mode();
    int resume = argc == 2 && !strcmp(argv[1], "--resume");
    int rearm = argc == 2 && !strcmp(argv[1], "--rearm");
    CHECK(argc == 1 || ((resume || rearm) && retry_mode == 2));
    if (retry_mode) return persistent(argv[0], retry_mode, resume, rearm);
    CHECK(umi_system_fixture_allowed());
    CHECK(!mkdir(UMI_SYSTEM_FIXTURE_ROOT, 0700));
    int directory = open(UMI_SYSTEM_FIXTURE_ROOT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(directory >= 0 && !mkdirat(directory, "keybags", 0700) &&
          !mkdirat(directory, "journal", 0700) && !fsync(directory));
    int journal = openat(directory, "journal", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct tj_store store;
    CHECK(journal >= 0 && !tj_open(&store, journal, policy));
    CHECK(!open_connection(&connection));
    uint64_t zero = 0, flags = ~0ull, mode[] = {0x554d4e41, 1};
    CHECK(!call(7, &zero, 1, NULL, 0, &flags, NULL, NULL) && flags == 6);
    CHECK(!call(0x7c, mode, 2, NULL, 0, NULL, NULL, NULL));

    CHECK(!request_change(argv[0], -1, 0));
    uint8_t initial[136], current[136], wrapped[48];
    uint64_t sequence = snapshot(&store, 1, 0, initial);
    expect_active(initial);
    struct item_keys keys = {0};
    CHECK(!ik_auth(&keys, initial, pins[0], strlen(pins[0])));
    size_t size = sizeof(wrapped);
    CHECK(!ik_wrap(&keys, 6, fixture_key, sizeof(fixture_key), wrapped, &size) && size == sizeof(wrapped));
    ik_clear(&keys);

    CHECK(request_change(argv[0], 0, -1) != 0);
    CHECK(snapshot(&store, 1, 0, current) == sequence && !memcmp(initial, current, 136));
    expect_active(current);
    check_protected(&keys, current, 0, wrapped);
    puts("UMI system transaction: empty new password refused; active bag and protected fixture preserved PASS");

    for (unsigned failures = 1; failures <= 5; ++failures) {
        CHECK(request_change(argv[0], 3, 1) != 0);
        snapshot(&store, 1, failures, current);
        if (failures < 5) usleep(rp_delay_for(failures) * 1000u);
    }
    sequence = snapshot(&store, 1, 5, current);
    CHECK(request_change(argv[0], 0, 1) != 0);
    CHECK(snapshot(&store, 1, 5, current) == sequence);
    expect_active(initial);
    sleep(5);
    puts("UMI system transaction: canonical change requests count wrong old passwords and enforce persistent wait PASS");

    fault(directory, "retry-crash-debit", 1);
    CHECK(request_change(argv[0], 0, 1) != 0);
    fault(directory, "retry-crash-debit", 0);
    sequence = snapshot(&store, 1, 6, current);
    CHECK(!memcmp(initial, current, 136));
    CHECK(request_change(argv[0], 0, 1) != 0);
    CHECK(snapshot(&store, 1, 6, current) == sequence);
    expect_active(initial);
    sleep(rp_delay_for(6) / 1000u + 1);
    puts("UMI system transaction: daemon exit before verification preserved debit and restart wait PASS");

    fault(directory, "fail-change-save", 1);
    CHECK(request_change(argv[0], 0, 1) != 0);
    fault(directory, "fail-change-save", 0);
    snapshot(&store, 1, 0, current);
    CHECK(!memcmp(initial, current, 136));
    expect_active(initial);
    check_protected(&keys, current, 0, wrapped);

    fault(directory, "fail-after-commit", 1);
    CHECK(!request_change(argv[0], 0, 1));
    fault(directory, "fail-after-commit", 0);
    snapshot(&store, 2, 0, current);
    expect_active(current);
    check_protected(&keys, current, 1, wrapped);
    puts("UMI system transaction: failed save kept old native bag and confirmed commit kept item key PASS");

    fault(directory, "crash-after-commit", 1);
    CHECK(request_change(argv[0], 1, 2) != 0);
    fault(directory, "crash-after-commit", 0);
    snapshot(&store, 3, 0, current);
    CHECK(request_change(argv[0], 1, 3) != 0);
    snapshot(&store, 3, 1, current);
    expect_active(current);
    CHECK(!call(7, &zero, 1, NULL, 0, &flags, NULL, NULL) && flags == 1);
    CHECK(unlock(0, pins[1]) != 0 && !unlock(0, pins[2]));
    check_protected(&keys, current, 2, wrapped);
    puts("UMI system transaction: canonical daemon recovered locked committed bag; obsolete PIN refused and protected item intact PASS");

    mode[1] = 0;
    CHECK(!call(0x7c, mode, 2, NULL, 0, NULL, NULL, NULL));
    IOServiceClose(connection);
    tj_close(&store);
    close(journal);
    close(directory);
    wipe(initial, sizeof(initial));
    wipe(current, sizeof(current));
    wipe(wrapped, sizeof(wrapped));
    puts("UMI system transaction: CANONICAL FIXTURE ALL PASS");
    return 0;
}
