#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define PART "/dev/block/sde61"
#define SYS_PART "/sys/class/block/sde61/"
#define START_SECTOR 1966080ULL
#define SECTORS 126976ULL
#define SMALL_SECTORS 110592ULL

static int sys_number(const char *path, unsigned long long expected) {
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    unsigned long long value = 0;
    int match = fscanf(file, "%llu", &value) == 1 && value == expected;
    fclose(file);
    return match;
}

static void dm_init(struct dm_ioctl *io, size_t size, const char *name) {
    memset(io, 0, size);
    io->version[0] = 4;
    io->data_size = size;
    io->data_start = sizeof(*io);
    strcpy(io->name, name);
}

static int dm_remove(int fd, const char *name) {
    struct dm_ioctl io;
    dm_init(&io, sizeof(io), name);
    if (!ioctl(fd, DM_DEV_REMOVE, &io)) return 0;
    perror("remove test mapper");
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3 || strlen(argv[2]) != 8 || strspn(argv[2], "0123456789abcdef") != 8) return 2;
    const char *mode = argv[1];
    int read_only = !strcmp(mode, "ro") || !strcmp(mode, "ro-small");
    unsigned long long sectors = !strcmp(mode, "ro-small") ? SMALL_SECTORS : SECTORS;
    char name[64];
    snprintf(name, sizeof(name), "umi_wrapped_native_%s", argv[2]);
    int dm = open("/dev/device-mapper", O_RDWR | O_CLOEXEC);
    if (dm < 0) { perror("open mapper control"); return 1; }
    if (!strcmp(mode, "remove")) return dm_remove(dm, name);
    if (!read_only && strcmp(mode, "rw")) return 2;
    if (!sys_number(SYS_PART "start", START_SECTOR) || !sys_number(SYS_PART "size", SECTORS)) return 2;
    struct stat st;
    int actual_ro = 0, part = open(PART, O_RDONLY | O_CLOEXEC);
    unsigned long long actual_bytes;
    if (part < 0 || fstat(part, &st) || !S_ISBLK(st.st_mode) || ioctl(part, BLKROGET, &actual_ro) ||
        (!read_only && actual_ro) || ioctl(part, BLKGETSIZE64, &actual_bytes) || actual_bytes != SECTORS * 512) {
        fprintf(stderr, "Unexpected LUN4 window or protection flags\n");
        return 2;
    }
    close(part);
    char key[258];
    if (!fgets(key, sizeof(key), stdin)) return 2;
    key[strcspn(key, "\n")] = '\0';
    size_t keylen = strlen(key);
    if (keylen < 128 || keylen > 256 || keylen % 2 || strspn(key, "0123456789abcdef") != keylen) return 2;
    struct dm_ioctl io;
    dm_init(&io, sizeof(io), name);
    if (ioctl(dm, DM_DEV_CREATE, &io)) { perror("create test mapper"); return 1; }
    struct {
        struct dm_ioctl io;
        struct dm_target_spec spec;
        char args[512];
    } t;
    int rc = 1;
    dm_init(&t.io, sizeof(t), name);
    t.io.flags = read_only ? DM_READONLY_FLAG : 0;
    t.io.target_count = 1;
    strcpy(t.spec.target_type, "default-key");
    t.spec.length = sectors;
    int n = snprintf(t.args, sizeof(t.args), "aes-xts-plain64 %s 0 %s 0 3 sector_size:4096 iv_large_sectors wrappedkey_v0", key, PART);
    explicit_bzero(key, sizeof(key));
    if (n < 0 || (size_t)n >= sizeof(t.args)) goto done;
    t.spec.next = (sizeof(t.spec) + n + 1 + 7) & ~7;
    t.io.data_size = sizeof(t.io) + t.spec.next;
    if (ioctl(dm, DM_TABLE_LOAD, &t)) { perror("load wrapped-key test mapping"); goto done; }
    explicit_bzero(&t, sizeof(t));
    dm_init(&io, sizeof(io), name);
    if (ioctl(dm, DM_DEV_SUSPEND, &io)) { perror("activate test mapping"); goto done; }
    char path[64];
    snprintf(path, sizeof(path), "/dev/block/dm-%u", minor((dev_t)io.dev));
    int dev = -1;
    for (int i = 0; i < 40 && dev < 0; ++i) {
        dev = open(path, O_RDONLY | O_CLOEXEC);
        if (dev < 0) usleep(100000);
    }
    if (dev < 0) { perror("open test mapping"); goto done; }
    if (!fstat(dev, &st) && S_ISBLK(st.st_mode) && st.st_rdev == (dev_t)io.dev &&
        !ioctl(dev, BLKROGET, &actual_ro) && actual_ro == read_only &&
        !ioctl(dev, BLKGETSIZE64, &actual_bytes) && actual_bytes == sectors * 512) rc = 0;
    close(dev);
    if (!rc) puts(path);
    else fprintf(stderr, "Mapping size or read-only state differs\n");
done:
    explicit_bzero(&t, sizeof(t));
    if (rc) dm_remove(dm, name);
    close(dm);
    return rc;
}
