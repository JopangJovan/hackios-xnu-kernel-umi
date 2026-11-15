#include <fcntl.h>
#include <inttypes.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open test file"); return 1; }
    const unsigned max_extents = 128;
    struct fiemap *map = calloc(1, sizeof(*map) + max_extents * sizeof(struct fiemap_extent));
    if (!map) { close(fd); return 1; }
    map->fm_length = FIEMAP_MAX_OFFSET;
    map->fm_flags = FIEMAP_FLAG_SYNC;
    map->fm_extent_count = max_extents;
    int result = ioctl(fd, FS_IOC_FIEMAP, map);
    if (result) perror("FIEMAP");
    close(fd);
    if (result || !map->fm_mapped_extents || map->fm_mapped_extents > max_extents ||
        !(map->fm_extents[map->fm_mapped_extents - 1].fe_flags & FIEMAP_EXTENT_LAST)) {
        free(map);
        return 1;
    }
    uint64_t last_end = 0;
    for (unsigned i = 0; i < map->fm_mapped_extents; ++i) {
        const struct fiemap_extent *extent = &map->fm_extents[i];
        if (extent->fe_flags & (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC)) {
            free(map);
            return 1;
        }
        uint64_t end = extent->fe_physical + extent->fe_length;
        if (end > last_end) last_end = end;
    }
    free(map);
    printf("%" PRIu64 "\n", last_end);
    return 0;
}
