#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/blkpg.h>
#include <linux/fs.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DEVICE "/dev/block/sde"
#define PART 61
#define START (245760LL * 4096)
#define LENGTH (62LL * 1024 * 1024)

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int add = strcmp(argv[1], "add") == 0;
    if (!add && strcmp(argv[1], "remove")) return 2;
    int fd = open(DEVICE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("open native unit read-only"); return 1; }
    int sector = 0;
    unsigned long long bytes = 0;
    if (ioctl(fd, BLKSSZGET, &sector) || sector != 4096 ||
        ioctl(fd, BLKGETSIZE64, &bytes) || bytes != 1024ULL * 1024 * 1024) {
        fprintf(stderr, "Unexpected LUN geometry\n");
        close(fd);
        return 1;
    }
    struct blkpg_partition part = {.start = START, .length = LENGTH, .pno = PART};
    struct blkpg_ioctl_arg op = {.op = add ? BLKPG_ADD_PARTITION : BLKPG_DEL_PARTITION,
        .datalen = sizeof(part), .data = &part};
    int result = ioctl(fd, BLKPG, &op);
    if (result < 0) perror("in-memory block view");
    close(fd);
    return result < 0;
}
