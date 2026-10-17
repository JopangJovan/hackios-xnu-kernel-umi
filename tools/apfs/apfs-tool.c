/*
 * Run the kernel's APFS reading code on the build machine.
 *
 * bsd/miscfs/apfs/apfs_lib.c is built into this program unchanged, so what it says about
 * an image here is what the driver will see.  `census` prints a line per directory entry
 * that tools/apfs/apfs.py can print too, and run-tests.sh diffs the two: the Python
 * reader has already been checked against 7-Zip over the whole volume, so agreeing with
 * it byte for byte is the C code agreeing with the image.
 *
 * Built by tools/apfs/build-tool.sh.
 *
 * usage: apfs-tool info   IMAGE
 *        apfs-tool ls     IMAGE [PATH]
 *        apfs-tool stat   IMAGE PATH
 *        apfs-tool cat    IMAGE PATH
 *        apfs-tool census IMAGE [PATH] [--meta]   (--meta: do not read contents)
 */

/* pread(2) and clock_gettime(2) come with -D_DEFAULT_SOURCE, set by build-tool.sh */
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apfs_lib.h"

/* ---- the image as a device ---- */

static int
file_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	int fd = (int)(intptr_t)ctx;
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = pread(fd, p, len, (off_t)off);

		if (n <= 0) {
			return EIO;
		}
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
	return 0;
}

/* The same hash the Python side computes, so contents can be compared without the bytes. */
static uint64_t
fnv1a(const uint8_t *p, size_t n, uint64_t h)
{
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

#define FNV_INIT 0xcbf29ce484222325ULL

static int
open_image(const char *path, struct apfs_nx *nx, struct apfs_vol *vol)
{
	struct apfs_dev dev;
	int fd = open(path, O_RDONLY);
	uint32_t i;
	int error;

	if (fd < 0) {
		perror(path);
		return 1;
	}
	memset(&dev, 0, sizeof(dev));
	dev.ctx = (void *)(intptr_t)fd;
	dev.size = (uint64_t)lseek(fd, 0, SEEK_END);
	dev.read = file_read;

	error = apfs_nx_open(nx, &dev);
	if (error) {
		fprintf(stderr, "apfs-tool: cannot read the container: %s\n", strerror(error));
		return 1;
	}
	/* the first volume that has a root directory, like apfs.py picks it */
	for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS; i++) {
		struct apfs_inode ino;

		if (nx->fs_oid[i] == 0) {
			continue;
		}
		if (apfs_vol_open(nx, i, vol) != 0) {
			continue;
		}
		if (apfs_inode_get(vol, APFS_ROOT_DIR_INO_NUM, &ino) == 0) {
			return 0;
		}
	}
	fprintf(stderr, "apfs-tool: no readable volume with a root directory\n");
	return 1;
}

/* ---- info ---- */

static int
cmd_info(const char *path)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	uint32_t i;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	printf("%s: APFS container, block size %u, %" PRIu64 " blocks\n", path,
	    nx.block_size, nx.block_count);
	printf("  newest checkpoint xid %" PRIu64 "\n", nx.xid);
	printf("  container omap block %" PRIu64 "\n", nx.omap_addr);
	for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS; i++) {
		struct apfs_vol v;

		if (nx.fs_oid[i] == 0 || apfs_vol_open(&nx, i, &v) != 0) {
			continue;
		}
		printf("  [%u] %s  role 0x%04x  incompatible 0x%" PRIx64 "  fs flags 0x%" PRIx64 "\n",
		    i, v.name, v.role, v.incompat, v.fs_flags);
		printf("       files %" PRIu64 "  dirs %" PRIu64 "  symlinks %" PRIu64
		    "  blocks used %" PRIu64 "\n", v.num_files, v.num_dirs, v.num_symlinks,
		    v.alloc_count);
		printf("       tree: %" PRIu64 " records in %" PRIu64 " nodes, flags 0x%x\n",
		    v.tree.key_count, v.tree.node_count, v.tree.flags);
	}
	printf("  blocks read %" PRIu64 ", cache hits %" PRIu64 "\n", nx.reads, nx.hits);
	apfs_nx_close(&nx);
	return 0;
}

/* ---- ls ---- */

struct ls_ctx {
	struct apfs_vol *vol;
};

static const char *
mode_kind(uint16_t mode)
{
	switch (mode & APFS_S_IFMT) {
	case APFS_S_IFDIR:  return "d";
	case APFS_S_IFLNK:  return "l";
	case APFS_S_IFREG:  return "-";
	case APFS_S_IFIFO:  return "p";
	case APFS_S_IFCHR:  return "c";
	case APFS_S_IFBLK:  return "b";
	case APFS_S_IFSOCK: return "s";
	default:            return "?";
	}
}

static int
ls_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct ls_ctx *lc = ctx;
	struct apfs_inode ino;

	(void)name_len;
	(void)d_type;
	if (apfs_inode_get(lc->vol, file_id, &ino) != 0) {
		printf("?  %20s  %s  (no inode record)\n", "", name);
		return 0;
	}
	printf("%s %06o %4u:%-4u %12" PRIu64 "  %-20" PRIu64 " %s", mode_kind(ino.mode),
	    ino.mode & 07777, ino.owner, ino.group, ino.size, file_id, name);
	if ((ino.mode & APFS_S_IFMT) == APFS_S_IFLNK) {
		char target[1024];
		size_t n = 0;

		if (apfs_readlink(lc->vol, file_id, target, sizeof(target) - 1, &n) == 0) {
			target[n] = '\0';
			printf(" -> %s", target);
		}
	}
	printf("\n");
	return 0;
}

static int
cmd_ls(const char *path, const char *where)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct ls_ctx lc;
	uint64_t id;
	uint8_t dt;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	error = apfs_path_lookup(&vol, where, &id, &dt);
	if (error) {
		fprintf(stderr, "apfs-tool: %s: %s\n", where, strerror(error));
		return 1;
	}
	lc.vol = &vol;
	if (dt != APFS_DT_DIR) {
		ls_cb(&lc, where, strlen(where), id, dt);
	} else {
		error = apfs_readdir(&vol, id, ls_cb, &lc);
		if (error) {
			fprintf(stderr, "apfs-tool: %s\n", strerror(error));
			return 1;
		}
	}
	apfs_nx_close(&nx);
	return 0;
}

/* ---- stat ---- */

static int
xattr_print_cb(void *ctx, const char *name, size_t name_len, const struct apfs_xattr *xa)
{
	(void)ctx;
	(void)name_len;
	printf("  xattr %-28s flags 0x%x  %u bytes%s\n", name, xa->flags, xa->data_len,
	    (xa->flags & APFS_XATTR_DATA_STREAM) ? " (in a data stream)" : "");
	return 0;
}

static int
cmd_stat(const char *path, const char *where)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_inode ino;
	struct apfs_cfile cf;
	uint64_t id;
	uint8_t dt;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	error = apfs_path_lookup(&vol, where, &id, &dt);
	if (error) {
		fprintf(stderr, "apfs-tool: %s: %s\n", where, strerror(error));
		return 1;
	}
	error = apfs_inode_get(&vol, id, &ino);
	if (error) {
		fprintf(stderr, "apfs-tool: inode %" PRIu64 ": %s\n", id, strerror(error));
		return 1;
	}
	printf("path        %s\n", where);
	printf("inode       %" PRIu64 "  parent %" PRIu64 "  private %" PRIu64 "\n",
	    ino.obj_id, ino.parent_id, ino.private_id);
	printf("mode        %06o  uid %u  gid %u\n", ino.mode, ino.owner, ino.group);
	printf("size        %" PRIu64 "  allocated %" PRIu64 "\n", ino.size,
	    ino.alloced_size);
	printf("flags       internal 0x%" PRIx64 "  bsd 0x%x%s\n", ino.internal_flags,
	    ino.bsd_flags, ino.is_sparse ? "  sparse" : "");
	if (apfs_cfile_init(&vol, &ino, &cf) == 0 && cf.compressed) {
		printf("decmpfs     type %u  uncompressed %" PRIu64 "  %s\n", cf.type, cf.size,
		    cf.in_rsrc ? "in the resource fork" : "in the attribute");
	}
	apfs_xattr_list(&vol, id, xattr_print_cb, NULL);
	apfs_nx_close(&nx);
	return 0;
}

/* ---- cat ---- */

static int
cmd_cat(const char *path, const char *where)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct apfs_inode ino;
	struct apfs_cfile cf;
	uint64_t id, off = 0;
	uint8_t dt;
	uint8_t *buf;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	error = apfs_path_lookup(&vol, where, &id, &dt);
	if (error == 0) {
		error = apfs_inode_get(&vol, id, &ino);
	}
	if (error) {
		fprintf(stderr, "apfs-tool: %s: %s\n", where, strerror(error));
		return 1;
	}
	buf = malloc(65536);
	if (buf == NULL) {
		return 1;
	}
	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	while (off < ino.size) {
		size_t got = 0;

		error = apfs_read_file(&vol, &ino, &cf, off, buf, 65536, &got);
		if (error) {
			fprintf(stderr, "apfs-tool: read at %" PRIu64 ": %s\n", off,
			    strerror(error));
			free(buf);
			return 1;
		}
		if (got == 0) {
			break;
		}
		if (fwrite(buf, 1, got, stdout) != got) {
			free(buf);
			return 1;
		}
		off += got;
	}
	apfs_cfile_fini(&cf);
	free(buf);
	apfs_nx_close(&nx);
	return 0;
}

/* ---- census: one line per entry, for diffing against apfs.py ---- */

struct census {
	struct apfs_vol *vol;
	char            *path;          /* the directory being walked, with a trailing / */
	size_t           path_len;
	size_t           path_cap;
	uint8_t         *buf;
	long             max;
	long             seen;
	size_t           chunk;         /* how much each read asks for */
	bool             meta_only;
	int              error;
};

static int census_dir(struct census *cs, uint64_t dir_id);

static uint64_t
file_hash(struct census *cs, const struct apfs_inode *ino, uint64_t size)
{
	uint64_t h = FNV_INIT, off = 0;
	struct apfs_cfile cf;

	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	for (;;) {
		size_t got = 0;
		int error = apfs_read_file(cs->vol, ino, &cf, off, cs->buf, cs->chunk, &got);

		if (error) {
			fprintf(stderr, "apfs-tool: %s: read at %" PRIu64 ": %s\n", cs->path,
			    off, strerror(error));
			cs->error = error;
			apfs_cfile_fini(&cf);
			return 0;
		}
		if (got == 0) {
			break;
		}
		h = fnv1a(cs->buf, got, h);
		off += got;
		if (off >= size) {
			break;
		}
	}
	apfs_cfile_fini(&cf);
	return h;
}

static int
census_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id, uint8_t d_type)
{
	struct census *cs = ctx;
	struct apfs_inode ino;
	size_t saved = cs->path_len;
	uint64_t h = 0, size = 0;
	int error;

	if (cs->max > 0 && cs->seen >= cs->max) {
		return EEXIST;
	}
	/* append "/name" to the path we are at */
	while (cs->path_len + name_len + 2 > cs->path_cap) {
		cs->path_cap *= 2;
		cs->path = realloc(cs->path, cs->path_cap);
		if (cs->path == NULL) {
			return ENOMEM;
		}
	}
	cs->path[cs->path_len++] = '/';
	memcpy(cs->path + cs->path_len, name, name_len);
	cs->path_len += name_len;
	cs->path[cs->path_len] = '\0';

	error = apfs_inode_get(cs->vol, file_id, &ino);
	if (error) {
		printf("%s\t?\tno-inode\n", cs->path);
		cs->error = error;
		goto done;
	}
	cs->seen++;
	if (apfs_inode_size(cs->vol, &ino, &size) != 0) {
		size = ino.size;
	}
	if (!cs->meta_only && ((ino.mode & APFS_S_IFMT) == APFS_S_IFREG ||
	    (ino.mode & APFS_S_IFMT) == APFS_S_IFLNK)) {
		h = file_hash(cs, &ino, size);
	}
	printf("%s\t%u\t%06o\t%u\t%u\t%" PRIu64 "\t%016" PRIx64 "\n", cs->path, d_type,
	    ino.mode, ino.owner, ino.group, size, h);

	if (d_type == APFS_DT_DIR) {
		error = census_dir(cs, file_id);
		if (error) {
			cs->path_len = saved;
			cs->path[cs->path_len] = '\0';
			return error;
		}
	}
done:
	cs->path_len = saved;
	cs->path[cs->path_len] = '\0';
	return 0;
}

static int
census_dir(struct census *cs, uint64_t dir_id)
{
	return apfs_readdir(cs->vol, dir_id, census_cb, cs);
}

static int
cmd_census(const char *path, const char *where, bool meta_only)
{
	struct apfs_nx nx;
	struct apfs_vol vol;
	struct census cs;
	uint64_t start = APFS_ROOT_DIR_INO_NUM;
	uint8_t dt = APFS_DT_DIR;
	int error;

	if (open_image(path, &nx, &vol) != 0) {
		return 1;
	}
	if (where != NULL && apfs_path_lookup(&vol, where, &start, &dt) != 0) {
		fprintf(stderr, "apfs-tool: %s: not found\n", where);
		return 1;
	}
	memset(&cs, 0, sizeof(cs));
	cs.vol = &vol;
	cs.path_cap = 4096;
	cs.path = malloc(cs.path_cap);
	/*
	 * APFS_TOOL_CHUNK: reads of another size than 64 KB (say 4096, or 12345 and 200000,
	 * which never line up with a compressed file's 64 KB pieces), so that the roads through
	 * apfs_read_file the census does not otherwise take are checked over a whole volume too.
	 */
	cs.chunk = 65536;
	if (getenv("APFS_TOOL_CHUNK") != NULL) {
		long c = strtol(getenv("APFS_TOOL_CHUNK"), NULL, 0);

		if (c < 1 || c > 16 * 1024 * 1024) {
			fprintf(stderr, "apfs-tool: APFS_TOOL_CHUNK %ld is not 1..16M\n", c);
			return 2;
		}
		cs.chunk = (size_t)c;
	}
	cs.buf = malloc(cs.chunk);
	cs.max = 0;
	cs.meta_only = meta_only;
	if (cs.path == NULL || cs.buf == NULL) {
		return 1;
	}
	cs.path[0] = '\0';
	if (where != NULL) {
		size_t n = strlen(where);

		while (n > 0 && where[n - 1] == '/') {
			n--;
		}
		memcpy(cs.path, where, n);
		cs.path_len = n;
		cs.path[n] = '\0';
	}
	error = census_dir(&cs, start);
	if (error && error != EEXIST) {
		fprintf(stderr, "apfs-tool: census: %s\n", strerror(error));
		return 1;
	}
	fprintf(stderr, "apfs-tool: %ld entries, %" PRIu64 " blocks read, %" PRIu64
	    " cache hits\n", cs.seen, nx.reads, nx.hits);
	fprintf(stderr, "apfs-tool: inflate %" PRIu64 " calls, %" PRIu64 " bytes in, %" PRIu64
	    " bytes out, %" PRIu64 " by zlib\n", nx.inflate_calls, nx.inflate_in, nx.inflate_out,
	    nx.inflate_fallbacks);
	fprintf(stderr, "apfs-tool: highest block touched %" PRIu64 " (%.2f GB in), of which "
	    "file contents reach %" PRIu64 " (%.2f GB in)\n", nx.max_addr,
	    nx.max_addr * (double)nx.block_size / 1e9, nx.max_data_addr,
	    nx.max_data_addr * (double)nx.block_size / 1e9);
	free(cs.path);
	free(cs.buf);
	apfs_nx_close(&nx);
	return cs.error ? 1 : 0;
}

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr,
		    "usage: apfs-tool info   IMAGE\n"
		    "       apfs-tool ls     IMAGE [PATH]\n"
		    "       apfs-tool stat   IMAGE PATH\n"
		    "       apfs-tool cat    IMAGE PATH\n"
		    "       apfs-tool census IMAGE [PATH] [--meta]\n");
		return 2;
	}
	if (strcmp(argv[1], "info") == 0) {
		return cmd_info(argv[2]);
	}
	if (strcmp(argv[1], "ls") == 0) {
		return cmd_ls(argv[2], argc > 3 ? argv[3] : "/");
	}
	if (strcmp(argv[1], "stat") == 0 && argc > 3) {
		return cmd_stat(argv[2], argv[3]);
	}
	if (strcmp(argv[1], "cat") == 0 && argc > 3) {
		return cmd_cat(argv[2], argv[3]);
	}
	if (strcmp(argv[1], "census") == 0) {
		return cmd_census(argv[2], (argc > 3 && argv[3][0] == '/') ? argv[3] : NULL,
		    (argc > 3 && strcmp(argv[argc - 1], "--meta") == 0));
	}
	fprintf(stderr, "apfs-tool: unknown command %s\n", argv[1]);
	return 2;
}
