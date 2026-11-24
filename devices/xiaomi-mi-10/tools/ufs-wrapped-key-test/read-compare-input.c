/* AOSP VTS metadata-decryption readback, with an explicitly read-only DM table. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <limits.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define RAW "/dev/block/sda34"
#define SAMPLE (1024 * 1024)
#define SECTORS (SAMPLE / 512)
#define PREFIX "umi_wrapped_vts_"

static void init(struct dm_ioctl *io, size_t size, const char *name) {
    memset(io, 0, size);
    io->version[0] = 4;
    io->data_size = size;
    io->data_start = sizeof(*io);
    strcpy(io->name, name);
}

static int read_sample(int fd, void *buf) {
    return pread(fd, buf, SAMPLE, 0) == SAMPLE;
}

static int send_sample(const void *buf) {
    const char *p = buf;
    size_t n = SAMPLE;
    while (n) {
        ssize_t sent = write(STDOUT_FILENO, p, n);
        if (sent <= 0) return 0;
        p += sent;
        n -= sent;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    signal(SIGPIPE, SIG_IGN);
    const char *mode = argv[1];
    int wrapped = !strcmp(mode, "wrapped");
    int software = !strcmp(mode, "software");
    if (!wrapped && !software && strcmp(mode, "inline-raw")) return 2;
    const char *suffix = argv[2];
    if (strlen(suffix) != 8 || strspn(suffix, "0123456789abcdef") != 8) return 2;

    char resolved[PATH_MAX];
    if (!realpath("/dev/block/by-name/userdata", resolved) || strcmp(resolved, RAW)) {
        fprintf(stderr, "Unexpected device for UMI userdata\n");
        return 2;
    }
    char key[258];
    if (!fgets(key, sizeof(key), stdin)) return 2;
    key[strcspn(key, "\n")] = '\0';
    size_t key_len = strlen(key);
    if (strspn(key, "0123456789abcdef") != key_len || key_len < 128 || key_len > 256 ||
        (key_len % 2) || (!wrapped && key_len != 128))
        return 2;

    int fd = open("/dev/device-mapper", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open device mapper"); return 2; }
    char name[64];
    snprintf(name, sizeof(name), "%s%s", PREFIX, suffix);
    struct dm_ioctl io;
    init(&io, sizeof(io), name);
    snprintf(io.uuid, sizeof(io.uuid), "UMI-WRAPPED-VTS-%s", suffix);
    if (ioctl(fd, DM_DEV_CREATE, &io) < 0) { perror("DM_DEV_CREATE"); close(fd); return 2; }

    int result = 1, raw = -1, mapped = -1;
    void *ciphertext = NULL, *plaintext = NULL, *after = NULL;
    struct {
        struct dm_ioctl io;
        struct dm_target_spec spec;
        char params[512];
    } table;
    init(&table.io, sizeof(table), name);
    table.io.flags = DM_READONLY_FLAG;
    table.io.target_count = 1;
    strcpy(table.spec.target_type, software ? "crypt" : "default-key");
    table.spec.length = SECTORS;
    int n = snprintf(table.params, sizeof(table.params), "aes-xts-plain64 %s 0 " RAW
                     " 0 %d sector_size:4096 iv_large_sectors%s",
                     key, wrapped ? 3 : 2, wrapped ? " wrappedkey_v0" : "");
    explicit_bzero(key, sizeof(key));
    table.spec.next = (sizeof(table.spec) + n + 1 + 7) & ~7u;
    table.io.data_size = sizeof(table.io) + table.spec.next;
    if (ioctl(fd, DM_TABLE_LOAD, &table) < 0) {
        perror("read-only DM_TABLE_LOAD");
        goto cleanup;
    }
    explicit_bzero(&table, sizeof(table));
    init(&io, sizeof(io), name);
    if (ioctl(fd, DM_DEV_SUSPEND, &io) < 0) { perror("activate test mapping"); goto cleanup; }

    dev_t mapdev = (dev_t)io.dev;
    unsigned minor_num = minor(mapdev);
    char path[128];
    snprintf(path, sizeof(path), "/dev/block/dm-%u", minor_num);
    for (int i = 0; i < 30 && mapped < 0; i++) {
        mapped = open(path, O_RDONLY | O_DIRECT | O_CLOEXEC);
        if (mapped < 0) usleep(100000);
    }
    if (mapped < 0) { perror("open test mapping read-only"); goto cleanup; }
    struct stat st;
    int ro = 0;
    uint64_t size = 0;
    if (fstat(mapped, &st) || !S_ISBLK(st.st_mode) || st.st_rdev != mapdev ||
        ioctl(mapped, BLKROGET, &ro) || ro != 1 || ioctl(mapped, BLKGETSIZE64, &size) || size != SAMPLE) {
        fprintf(stderr, "Expected a verified read-only 1 MiB DM device\n");
        goto cleanup;
    }
    fprintf(stderr, "Kernel confirmed the %s mapping is read-only\n", mode);

    raw = open(RAW, O_RDONLY | O_DIRECT | O_CLOEXEC);
    if (raw < 0) { perror("open native UFS read-only"); goto cleanup; }
    if (posix_memalign(&ciphertext, 4096, SAMPLE) || posix_memalign(&plaintext, 4096, SAMPLE) ||
        posix_memalign(&after, 4096, SAMPLE)) {
        fprintf(stderr, "sample allocation failed\n");
        goto cleanup;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!read_sample(raw, ciphertext) || !read_sample(mapped, plaintext) || !read_sample(raw, after)) {
            perror("read-only UFS or DM read");
            goto cleanup;
        }
        if (!memcmp(ciphertext, after, SAMPLE)) {
            if (send_sample(ciphertext) && send_sample(plaintext)) result = 0;
            goto cleanup;
        }
    }
    fprintf(stderr, "Raw sectors changed while sampling\n");

cleanup:
    explicit_bzero(&table, sizeof(table));
    if (ciphertext) { explicit_bzero(ciphertext, SAMPLE); free(ciphertext); }
    if (plaintext) { explicit_bzero(plaintext, SAMPLE); free(plaintext); }
    if (after) { explicit_bzero(after, SAMPLE); free(after); }
    if (mapped >= 0) close(mapped);
    if (raw >= 0) close(raw);
    init(&io, sizeof(io), name);
    if (ioctl(fd, DM_DEV_REMOVE, &io) < 0) { perror("remove temporary mapping"); result = 1; }
    else fprintf(stderr, "Removed %s test mapping\n", mode);
    close(fd);
    return result;
}
