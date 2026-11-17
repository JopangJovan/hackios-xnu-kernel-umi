/* Isolated crypto capability test, not Apple's keybag format or a passcode API.
 * Uses only synthetic, public test data and a public test passphrase. Never
 * point this at /var/keybags or at existing user data. No block-device I/O.
 */
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <CommonCrypto/CommonRandom.h>
#include <CommonCrypto/CommonSymmetricKeywrap.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { BAG_SIZE = 152, BAG_MAC = 120, ROUNDS = 600000,
       DATA_SIZE = 16384, DATA_HEADER = 32,
       SEALED_SIZE = DATA_HEADER + DATA_SIZE + 16 + 32 };
static const char test_password[] = "UMI-public-crypto-selftest-only-v1";
static const char next_password[] = "UMI-public-crypto-selftest-only-v2";
static const unsigned char bag_magic[] = "UMIKBT01";
static const unsigned char file_magic[] = "UMIDAT01";

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}
static int equal(const unsigned char *a, const unsigned char *b, size_t n) {
    volatile unsigned char delta = 0;
    while (n--) delta |= *a++ ^ *b++;
    return delta == 0;
}
static void put32(unsigned char *p, uint32_t v) {
    for (int n = 3; n >= 0; --n) { p[n] = v; v >>= 8; }
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static int derive(const char *password, const unsigned char salt[32], unsigned char out[64]) {
    return CCKeyDerivationPBKDF(kCCPBKDF2, password, strlen(password), salt, 32,
                               kCCPRFHmacAlgSHA256, ROUNDS, out, 64);
}
static int make_bag(unsigned char bag[BAG_SIZE], const unsigned char keys[64],
                    const char *password) {
    unsigned char derived[64] = {0};
    size_t wrapped = 72;
    int rc = -1;
    memset(bag, 0, BAG_SIZE);
    memcpy(bag, bag_magic, 8);
    put32(bag + 8, 1);
    put32(bag + 12, ROUNDS);
    if (CCRandomGenerateBytes(bag + 16, 32) || derive(password, bag + 16, derived)) goto done;
    if (CCSymmetricKeyWrap(kCCWRAPAES, CCrfc3394_iv, CCrfc3394_ivLen,
                          derived, 32, keys, 64, bag + 48, &wrapped) || wrapped != 72) goto done;
    CCHmac(kCCHmacAlgSHA256, derived + 32, 32, bag, BAG_MAC, bag + BAG_MAC);
    rc = 0;
done:
    wipe(derived, sizeof(derived));
    if (rc) wipe(bag, BAG_SIZE);
    return rc;
}
static int open_bag(const unsigned char *bag, size_t bytes, const char *password,
                    unsigned char keys[64]) {
    unsigned char derived[64] = {0}, tag[32] = {0};
    size_t unwrapped = 64;
    int rc = -1;
    wipe(keys, 64);
    if (bytes != BAG_SIZE || memcmp(bag, bag_magic, 8) || get32(bag + 8) != 1 ||
        get32(bag + 12) != ROUNDS) goto done;
    if (derive(password, bag + 16, derived)) goto done;
    CCHmac(kCCHmacAlgSHA256, derived + 32, 32, bag, BAG_MAC, tag);
    if (!equal(tag, bag + BAG_MAC, 32)) goto done;
    if (CCSymmetricKeyUnwrap(kCCWRAPAES, CCrfc3394_iv, CCrfc3394_ivLen,
                            derived, 32, bag + 48, 72, keys, &unwrapped) || unwrapped != 64) goto done;
    rc = 0;
done:
    wipe(derived, sizeof(derived));
    wipe(tag, sizeof(tag));
    if (rc) wipe(keys, 64);
    return rc;
}
static void plaintext(unsigned char data[DATA_SIZE]) {
    for (size_t i = 0; i < DATA_SIZE; ++i) data[i] = (unsigned char)(i * 17 + i / 251);
}
static int seal_data(const unsigned char keys[64], unsigned char sealed[SEALED_SIZE]) {
    unsigned char data[DATA_SIZE];
    size_t bytes = 0;
    int rc = -1;
    plaintext(data);
    memcpy(sealed, file_magic, 8);
    put32(sealed + 8, 1);
    put32(sealed + 12, DATA_SIZE);
    if (CCRandomGenerateBytes(sealed + 16, 16)) goto done;
    if (CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, keys, 32,
                sealed + 16, data, sizeof(data), sealed + DATA_HEADER,
                DATA_SIZE + 16, &bytes) || bytes != DATA_SIZE + 16) goto done;
    CCHmac(kCCHmacAlgSHA256, keys + 32, 32, sealed, SEALED_SIZE - 32,
           sealed + SEALED_SIZE - 32);
    rc = 0;
done:
    wipe(data, sizeof(data));
    if (rc) wipe(sealed, SEALED_SIZE);
    return rc;
}
static int open_data(const unsigned char keys[64], const unsigned char *sealed, size_t len,
                     unsigned char output[DATA_SIZE + 16]) {
    unsigned char tag[32];
    size_t bytes = 0;
    wipe(output, DATA_SIZE + 16);
    if (len != SEALED_SIZE || memcmp(sealed, file_magic, 8) ||
        get32(sealed + 8) != 1 || get32(sealed + 12) != DATA_SIZE) return -1;
    CCHmac(kCCHmacAlgSHA256, keys + 32, 32, sealed, len - 32, tag);
    int valid = equal(tag, sealed + len - 32, 32);
    wipe(tag, sizeof(tag));
    if (!valid) return -1; /* authenticate before decrypting or exposing plaintext */
    if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, keys, 32,
                sealed + 16, sealed + DATA_HEADER, DATA_SIZE + 16, output,
                DATA_SIZE + 16, &bytes) || bytes != DATA_SIZE) {
        wipe(output, DATA_SIZE + 16);
        return -1;
    }
    return 0;
}
static int read_file(int dir, const char *name, void *data, size_t len) {
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    int rc = -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0777) != 0600 || st.st_size != (off_t)len) goto done;
    size_t at = 0;
    while (at < len) {
        ssize_t n = read(fd, (unsigned char *)data + at, len - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        at += (size_t)n;
    }
    rc = 0;
done:
    close(fd);
    return rc;
}
static int write_new(int dir, const char *name, const void *data, size_t len) {
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t at = 0;
    int rc = -1;
    while (at < len) {
        ssize_t n = write(fd, (const unsigned char *)data + at, len - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        at += (size_t)n;
    }
    rc = fsync(fd);
done:
    if (close(fd)) rc = -1;
    return rc;
}
static int known_answer(void) {
    /* RFC 3394 section 4.1: AES-128 KEK, 128-bit wrapped key. */
    const unsigned char expected[24] = {0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,
        0xae,0xf3,0x4b,0xd8,0xfb,0x5a,0x7b,0x82,0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5};
    unsigned char kek[16], raw[16], wrapped[24];
    for (unsigned i = 0; i < 16; ++i) { kek[i] = i; raw[i] = i * 0x11; }
    size_t len = sizeof(wrapped);
    return CCSymmetricKeyWrap(kCCWRAPAES, CCrfc3394_iv, CCrfc3394_ivLen,
                              kek, 16, raw, 16, wrapped, &len) ||
           len != sizeof(expected) || !equal(wrapped, expected, len);
}

/* Single-writer, synthetic-fixture experiment. A real key store also needs
 * concurrency control, durable anti-rollback state, and power-loss tests.
 * The active keybag is replaced only after the new encrypted bag is fsynced.
 * A failure after rename has an uncertain durability outcome: never report
 * success to a caller until the directory sync completes.
 */
static int rotate_test_bag(int dir, const char *mode) {
    unsigned char old[BAG_SIZE] = {0}, next[BAG_SIZE] = {0}, keys[64] = {0};
    int rc = -1;
    if (read_file(dir, "test-keybag.bin", old, sizeof(old)) ||
        open_bag(old, sizeof(old), test_password, keys) ||
        make_bag(next, keys, next_password)) goto done;
    wipe(keys, sizeof(keys));
    if (!strcmp(mode, "rotate-incomplete")) {
        if (write_new(dir, "test-keybag.next", next, BAG_SIZE / 2)) goto done;
        _exit(86); /* process dies with an incomplete, uncommitted ciphertext */
    }
    if (write_new(dir, "test-keybag.next", next, sizeof(next))) goto done;
    if (!strcmp(mode, "rotate-pre-rename")) _exit(86);
    if (renameat(dir, "test-keybag.next", dir, "test-keybag.bin")) goto done;
    if (!strcmp(mode, "rotate-post-rename")) _exit(86);
    if (fsync(dir)) goto done;
    rc = 0;
done:
    wipe(keys, sizeof(keys));
    wipe(old, sizeof(old));
    wipe(next, sizeof(next));
    return rc;
}

/* Never promote an uncommitted .next bag, even if it authenticates. The
 * caller must first authenticate the active file. This only removes this
 * program's pending test file from its private test directory.
 */
static int discard_pending_test(int dir) {
    unsigned char bag[BAG_SIZE] = {0}, keys[64] = {0};
    struct stat st;
    int rc = -1;
    if (read_file(dir, "test-keybag.bin", bag, sizeof(bag)) ||
        open_bag(bag, sizeof(bag), test_password, keys)) goto done;
    if (fstatat(dir, "test-keybag.next", &st, AT_SYMLINK_NOFOLLOW) ||
        !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0777) != 0600 ||
        (st.st_size != BAG_SIZE / 2 && st.st_size != BAG_SIZE)) goto done;
    if (unlinkat(dir, "test-keybag.next", 0) || fsync(dir)) goto done;
    rc = 0;
done:
    wipe(bag, sizeof(bag)); wipe(keys, sizeof(keys));
    return rc;
}

extern char **environ;
static int run_child(const char *self, const char *mode, const char *path, int expected) {
    char *args[] = {(char *)self, (char *)mode, (char *)path, NULL};
    pid_t pid;
    int status;
    if (posix_spawn(&pid, self, NULL, NULL, args, environ)) return -1;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    return !WIFEXITED(status) || WEXITSTATUS(status) != expected;
}

static int cipher_hash(const char *path, unsigned char digest[32]) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    unsigned char data[SEALED_SIZE];
    if (fd < 0) return -1;
    int rc = read_file(fd, "test-ciphertext.bin", data, sizeof(data));
    if (!rc) CC_SHA256(data, sizeof(data), digest);
    close(fd);
    return rc;
}

static int exercise(const char *self, const char *root, int verify_only) {
    const char *modes[] = {"rotate", "rotate-incomplete", "rotate-pre-rename", "rotate-post-rename"};
    if (!verify_only && mkdir(root, 0700)) return 1;
    int dir = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    struct stat st;
    if (dir < 0) return 1;
    if (fstat(dir, &st) || st.st_uid != getuid() || (st.st_mode & 0777) != 0700) {
        close(dir); return 1;
    }
    close(dir);
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        char path[PATH_MAX];
        unsigned char before[32], after[32];
        int n = snprintf(path, sizeof(path), "%s/%s", root, modes[i]);
        if (n < 0 || (size_t)n >= sizeof(path)) return 1;
        if (!verify_only) {
            if (run_child(self, "create", path, 0) || cipher_hash(path, before) ||
                run_child(self, modes[i], path, i ? 86 : 0)) return 1;
            if (i == 1 || i == 2) {
                if (run_child(self, "verify", path, 0) ||
                    run_child(self, "discard-pending", path, 0) ||
                    run_child(self, "rotate", path, 0)) return 1;
            }
            if (cipher_hash(path, after) || !equal(before, after, sizeof(before))) return 1;
            printf("UMI crypto test: %s kept data ciphertext unchanged\n", modes[i]);
        }
        if (run_child(self, "verify-rotated", path, 0)) return 1;
    }
    printf("UMI crypto test: %s PASS\n", verify_only ? "verify-exercise" : "exercise");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 3) return 2;
    if (!strcmp(argv[1], "exercise") || !strcmp(argv[1], "verify-exercise"))
        return exercise(argv[0], argv[2], !strcmp(argv[1], "verify-exercise"));
    const int rotated = !strcmp(argv[1], "verify-rotated");
    const int rotate = !strcmp(argv[1], "rotate") || !strcmp(argv[1], "rotate-incomplete") ||
        !strcmp(argv[1], "rotate-pre-rename") || !strcmp(argv[1], "rotate-post-rename");
    const int discard = !strcmp(argv[1], "discard-pending");
    if (strcmp(argv[1], "create") && strcmp(argv[1], "verify") && !rotated && !rotate && !discard) return 2;
    const char *password = rotated ? next_password : test_password;
    int create = !strcmp(argv[1], "create"), rc = 1, dir = -1;
    unsigned char bag[BAG_SIZE] = {0}, badbag[BAG_SIZE], keys[64] = {0}, rejected[64] = {0};
    unsigned char sealed[SEALED_SIZE], output[DATA_SIZE + 16], expected[DATA_SIZE];
    if (known_answer()) goto done;
    puts("UMI crypto test: RFC3394 known answer PASS");
    /* A fresh directory is required for create; never overwrite any prior test. */
    if (create && mkdir(argv[2], 0700)) goto done;
    dir = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    struct stat st;
    if (dir < 0 || fstat(dir, &st) || st.st_uid != getuid() || (st.st_mode & 0777) != 0700) goto done;
    if (rotate || discard) {
        rc = (rotate ? rotate_test_bag(dir, argv[1]) : discard_pending_test(dir)) ? 1 : 0;
        goto done;
    }
    if (create) {
        if (CCRandomGenerateBytes(keys, sizeof(keys)) || make_bag(bag, keys, password) || seal_data(keys, sealed)) goto done;
        if (write_new(dir, "test-keybag.bin", bag, sizeof(bag)) ||
            write_new(dir, "test-ciphertext.bin", sealed, sizeof(sealed)) || fsync(dir)) goto done;
        wipe(keys, sizeof(keys));
        puts("UMI crypto test: synthetic keybag and ciphertext saved");
    }
    if (read_file(dir, "test-keybag.bin", bag, sizeof(bag)) ||
        read_file(dir, "test-ciphertext.bin", sealed, sizeof(sealed))) goto done;
    if (!open_bag(bag, sizeof(bag), "deliberately-wrong-test-password", rejected)) goto done;
    unsigned char zeros[64] = {0};
    if (!equal(rejected, zeros, sizeof(zeros))) goto done;
    puts("UMI crypto test: wrong password rejected; no key returned");
    if (rotated) {
        if (!open_bag(bag, sizeof(bag), test_password, rejected) ||
            !equal(rejected, zeros, sizeof(zeros))) goto done;
        puts("UMI crypto test: previous password rejected after rotation");
    }
    memcpy(badbag, bag, sizeof(bag)); badbag[48] ^= 1;
    if (!open_bag(badbag, sizeof(badbag), password, rejected)) goto done;
    if (!equal(rejected, zeros, sizeof(zeros))) goto done;
    puts("UMI crypto test: tampered keybag rejected; no key returned");
    if (open_bag(bag, sizeof(bag), password, keys) ||
        open_data(keys, sealed, sizeof(sealed), output)) goto done;
    plaintext(expected);
    if (!equal(output, expected, sizeof(expected))) goto done;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(output, DATA_SIZE, digest);
    printf("UMI crypto test: decrypted payload SHA256 ");
    for (size_t i = 0; i < sizeof(digest); ++i) printf("%02x", digest[i]);
    putchar('\n');
    sealed[DATA_HEADER + 7] ^= 1;
    if (!open_data(keys, sealed, sizeof(sealed), output)) goto done;
    for (size_t i = 0; i < sizeof(output); ++i) if (output[i]) goto done;
    puts("UMI crypto test: tampered ciphertext rejected before plaintext release");
    rc = 0;
done:
    wipe(keys, sizeof(keys)); wipe(rejected, sizeof(rejected));
    wipe(output, sizeof(output)); wipe(expected, sizeof(expected));
    if (dir >= 0) close(dir);
    printf("UMI crypto test: %s %s\n", argc > 1 ? argv[1] : "invalid", rc ? "FAIL" : "PASS");
    return rc;
}
