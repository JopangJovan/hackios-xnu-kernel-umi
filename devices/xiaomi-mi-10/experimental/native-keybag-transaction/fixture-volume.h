/* A temporary loader publishes only this independent 8 MiB HFSX device. */
#ifndef UMI_FIXTURE_VOLUME_H
#define UMI_FIXTURE_VOLUME_H

#include <CommonCrypto/CommonDigest.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <zlib.h>
#include "system-fixture.h"

/* Darwin bsd/sys/disk.h is absent from the public iPhoneOS SDK. */
#define UMI_DK_GET_BLOCK_SIZE _IOR('d', 24, uint32_t)
#define UMI_DK_GET_BLOCK_COUNT _IOR('d', 25, uint64_t)
#define UMI_DK_SYNC_CACHE _IO('d', 22)

static int disk_io(int fd, uint8_t *bytes, size_t size, off_t offset, int writing)
{
    ssize_t amount;
    do {
        amount = writing ? pwrite(fd, bytes, size, offset) : pread(fd, bytes, size, offset);
    } while (amount < 0 && errno == EINTR);
    return amount == (ssize_t)size ? 0 : -1;
}

static int matches_rejected_fixture(int fd, uint8_t *buffer, size_t capacity)
{
    uint8_t expected[CC_SHA256_DIGEST_LENGTH], actual[sizeof(expected)];
    int record = open("/redwood/etc/umi-retry-replace.sha256", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (record < 0) return 0;
    struct stat st;
    int valid = !fstat(record, &st) && S_ISREG(st.st_mode) && st.st_size == sizeof(expected) &&
        !disk_io(record, expected, sizeof(expected), 0, 0);
    close(record);
    if (!valid) return 0;
    CC_SHA256_CTX hash;
    CC_SHA256_Init(&hash);
    for (off_t offset = 0; offset < UMI_SYSTEM_FIXTURE_BYTES; offset += capacity) {
        if (disk_io(fd, buffer, capacity, offset, 0)) return 0;
        CC_SHA256_Update(&hash, buffer, (CC_LONG)capacity);
    }
    CC_SHA256_Final(actual, &hash);
    return !memcmp(actual, expected, sizeof(actual));
}

static int mount_fixture_volume(int mode)
{
    static uint8_t block[65536], blank[65536];
    static const char template[] = "/redwood/etc/umi-retry-hfs.zlib";
    int fd = open(UMI_SYSTEM_FIXTURE_RAW, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    uint32_t sector = 0;
    uint64_t count = 0;
    int e = -1;
    int claimed = 0;
    const char *step = "read device geometry";
    if (fd < 0 || ioctl(fd, UMI_DK_GET_BLOCK_SIZE, &sector) || ioctl(fd, UMI_DK_GET_BLOCK_COUNT, &count) ||
        sector != 4096 || count != UMI_SYSTEM_FIXTURE_BYTES / sector) goto close_disk;
    step = "read first block";
    if (disk_io(fd, block, sizeof(block), 0, 0)) goto close_disk;
    step = "confirm expected fixture state";
    if (mode == 1) {
        if (!memcmp(block, blank, sizeof(block))) {
            for (off_t offset = sizeof(block); offset < UMI_SYSTEM_FIXTURE_BYTES; offset += sizeof(block)) {
                if (disk_io(fd, block, sizeof(block), offset, 0) || memcmp(block, blank, sizeof(block)))
                    goto close_disk;
            }
            puts("UMI shared retry: verified entire independent extent is zero");
        } else {
            if (memcmp(block + 1024, "HX", 2) || !matches_rejected_fixture(fd, block, sizeof(block))) {
                errno = EFTYPE;
                goto close_disk;
            }
            puts("UMI shared retry: all bytes match the rejected blank fixture");
        }
        step = "claim fixture raw writer";
        int claim = 1;
        if (sysctlbyname("kern.hakios.var_busy", NULL, NULL, &claim, sizeof(claim)))
            goto close_disk;
        claimed = 1;
        step = "prepare fixture image";
        close(fd);
        fd = open(UMI_SYSTEM_FIXTURE_RAW, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        int image = open(template, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st;
        if (fd < 0 || image < 0 || fstat(image, &st) || st.st_size <= 0 || st.st_size > 512 * 1024) {
            if (image >= 0) close(image);
            goto close_disk;
        }
        uint8_t *packed = malloc((size_t)st.st_size);
        uint8_t *plain = malloc(UMI_SYSTEM_FIXTURE_BYTES);
        int failed = !packed || !plain || disk_io(image, packed, (size_t)st.st_size, 0, 0);
        close(image);
        if (!failed) {
            step = "write fixture image";
            unsigned long n = UMI_SYSTEM_FIXTURE_BYTES;
            failed = uncompress(plain, &n, packed, (size_t)st.st_size) ||
                n != UMI_SYSTEM_FIXTURE_BYTES || memcmp(plain + 1024, "HX", 2);
        }
        if (!failed) {
            for (off_t offset = 0; offset < UMI_SYSTEM_FIXTURE_BYTES; offset += sizeof(block)) {
                if (disk_io(fd, plain + offset, sizeof(block), offset, 1)) { failed = 1; break; }
            }
            if (!failed) {
                step = "flush fixture device";
                failed = ioctl(fd, UMI_DK_SYNC_CACHE, NULL);
            }
        }
        free(plain);
        free(packed);
        if (failed) goto close_disk;
    } else if (mode == 1 || memcmp(block + 1024, "HX", 2)) {
        goto close_disk;
    }
    e = 0;
close_disk:
    {
        int error = errno;
        if (fd >= 0 && close(fd) && !e) { e = -1; error = errno; step = "close raw fixture"; }
        if (claimed) {
            int release = 0;
            if (sysctlbyname("kern.hakios.var_busy", NULL, NULL, &release, sizeof(release)) && !e) {
                e = -1;
                error = errno;
                step = "release fixture raw writer";
            }
        }
        if (e) {
            fprintf(stderr, "UMI shared retry: %s failed (sector=%u blocks=%llu errno=%d)\n", step, sector, count, error);
            return -1;
        }
    }
    if (mkdir(UMI_SYSTEM_FIXTURE_MOUNT, 0700)) return -1;
    struct {
        const char *fspec;
        uid_t uid;
        gid_t gid;
        mode_t mask;
        uint32_t encoding;
        struct timezone zone;
        int flags, journal_buffer, journal_flags, disable_journal;
    } args = {.fspec = UMI_SYSTEM_FIXTURE_DEVICE, .mask = 0700, .flags = 4};
    if (mount("hfs", UMI_SYSTEM_FIXTURE_MOUNT, MNT_NOSUID | MNT_NODEV, &args)) return -1;
    if (mode == 1 && chmod(UMI_SYSTEM_FIXTURE_MOUNT, 0700)) return -1;
    if (!umi_system_fixture_allowed()) { errno = EFTYPE; return -1; }
    puts("UMI shared retry: independent HFSX mounted with its journal");
    return 0;
}

#endif
