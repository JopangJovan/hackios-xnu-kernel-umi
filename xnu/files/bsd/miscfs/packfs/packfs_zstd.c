/*
 * packfs: chunks compressed with Zstandard (PACKFS_F_ZSTD).
 *
 * The decoder is zstddeclib.c, the single-file decoder of the reference implementation
 * (tools/mk-zstddec.sh of the redwood repository generates it). It is compiled as part of
 * this file, with the kernel standing in for the C library: it needs memcpy and friends,
 * which are there, and an allocator, which it never gets to call because the one
 * decompression context there is lives in memory of ours.
 */
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DEBUGLEVEL              0
#define ZSTD_NO_INTRINSICS      1       /* no NEON in the kernel without asking */
#define XXH_NO_STDLIB           1
#define ZSTD_DEPS_MALLOC        1       /* "the allocator is defined": this is it */
#define ZSTD_malloc(size)       ((void)(size), (void *)NULL)
#define ZSTD_calloc(n, size)    ((void)(n), (void)(size), (void *)NULL)
#define ZSTD_free(p)            ((void)(p))

#include "zstddeclib.c"

int packfs_zstd_decode(uint8_t *dst, size_t dst_size, const uint8_t *src, size_t src_size);

static LCK_GRP_DECLARE(packfs_zstd_grp, "packfs.zstd");
static LCK_MTX_DECLARE(packfs_zstd_lock, &packfs_zstd_grp);
static ZSTD_DCtx *packfs_zstd_context;

/*
 * Decompress one frame of exactly dst_size bytes into dst, which must be memory that cannot
 * fault (the caller's page cache mapping or bounce buffer): the context's lock is held.
 */
int
packfs_zstd_decode(uint8_t *dst, size_t dst_size, const uint8_t *src, size_t src_size)
{
	size_t done;

	lck_mtx_lock(&packfs_zstd_lock);
	if (packfs_zstd_context == NULL) {
		size_t size = ZSTD_estimateDCtxSize();

		packfs_zstd_context = ZSTD_initStaticDCtx(kalloc_data(size, Z_WAITOK | Z_ZERO), size);
		if (packfs_zstd_context == NULL) {
			lck_mtx_unlock(&packfs_zstd_lock);
			return ENOMEM;
		}
	}
	done = ZSTD_decompressDCtx(packfs_zstd_context, dst, dst_size, src, src_size);
	lck_mtx_unlock(&packfs_zstd_lock);
	return (!ZSTD_isError(done) && done == dst_size) ? 0 : EIO;
}
