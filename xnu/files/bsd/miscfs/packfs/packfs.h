/*
 * packfs: a read-only filesystem for a boot image.
 *
 * The open-source kernel has no on-disk filesystem of its own (HFS and APFS
 * are separate kexts, mockfs shows exactly one file), so the root of the
 * bring-up system lives in this deliberately simple format. An image is
 * built on the host by mkpackfs.py and consists of:
 *
 *   superblock | node table | name strings | file data
 *
 * Node 0 is the root directory. The children of a directory occupy a
 * contiguous run of the node table, sorted by name.
 *
 * A file is stored in one of two ways:
 *   - plain: one contiguous extent aligned to the image block size; the
 *     generic cluster layer does all the I/O through the block device;
 *   - chunked (PACKFS_F_CHUNKED): cut into chunks of PACKFS_CHUNK_SIZE bytes,
 *     each stored either compressed -- a raw LZ4 block, or with PACKFS_F_ZSTD
 *     a Zstandard frame, a quarter smaller and four times slower to read --
 *     or, when that does not make it smaller, as it is. The extent starts with a table of
 *     32-bit offsets, one per chunk and one more for the end, counted from
 *     the start of the extent; a chunk whose stored length equals its real
 *     length is not compressed. Chunked files are read straight out of the
 *     memory of a RAM disk, so an image that has them mounts only from a
 *     memory device. This is what lets a system that does not fit into RAM
 *     twice (once as the disk, once in the page cache) run from a RAM disk.
 *
 * Everything is little-endian. Nothing is ever written.
 */
#ifndef _MISCFS_PACKFS_H_
#define _MISCFS_PACKFS_H_

#include <stdint.h>

#define PACKFS_MAGIC            "PACKFS1"      /* 8 bytes with the terminator */
#define PACKFS_SUPER_SIZE       4096
#define PACKFS_ROOT_NODE        0
#define PACKFS_MAX_NAME         255

struct packfs_super {
	char     ps_magic[8];
	uint32_t ps_block_size;         /* alignment of file data; a power of two, at least 4096 */
	uint32_t ps_node_count;
	uint64_t ps_nodes_off;          /* byte offset of the node table */
	uint64_t ps_strings_off;        /* byte offset of the string area */
	uint64_t ps_strings_size;
	uint64_t ps_data_off;           /* byte offset of the first file extent */
	uint64_t ps_image_size;
	uint64_t ps_create_time;        /* seconds since the epoch */
};

struct packfs_dnode {
	uint32_t pd_mode;               /* file type and permission bits, as in st_mode */
	uint32_t pd_uid;
	uint32_t pd_gid;
	uint32_t pd_nlink;
	uint64_t pd_size;               /* file: bytes; symlink: target length; directory: number of children */
	uint64_t pd_data;               /* file: byte offset of the extent; symlink: offset of the target in the
	                                 * string area; directory: node index of the first child */
	uint32_t pd_parent;             /* node index of the containing directory (the root is its own parent) */
	uint32_t pd_name;               /* offset of the name in the string area */
	uint32_t pd_name_len;
	uint32_t pd_rdev;
	uint64_t pd_mtime;              /* seconds since the epoch */
	uint64_t pd_flags;              /* PACKFS_F_* */
};

#define PACKFS_F_CHUNKED        0x1     /* the extent is a chunk table followed by the chunks */
#define PACKFS_F_ZSTD           0x2     /* ... and the chunks are Zstandard frames, not LZ4 blocks */
#define PACKFS_CHUNK_SIZE       65536   /* a multiple of every page size we run with */

#ifdef KERNEL

#include <kern/locks.h>
#include <sys/mount.h>
#include <sys/vnode.h>

/* In-core state of one node: the vnode currently standing for it, if any. */
struct packfs_inode {
	vnode_t  pi_vp;
	uint32_t pi_vid;
	uint32_t pi_flags;
};
#define PACKFS_I_CREATING       0x1     /* a thread is in vnode_create() for this node */
#define PACKFS_I_WANTED         0x2     /* somebody sleeps on the node */

struct packfs_mount {
	mount_t                 pm_mp;
	vnode_t                 pm_devvp;
	struct packfs_super     pm_super;
	struct packfs_dnode    *pm_nodes;       /* the whole node table */
	char                   *pm_strings;     /* the whole string area */
	struct packfs_inode    *pm_inodes;      /* one per node */
	const uint8_t          *pm_membase;     /* the image in kernel memory, when the device is a RAM disk */
	uint64_t                pm_memsize;
	lck_mtx_t               pm_lock;        /* guards pm_inodes */
};

#define PACKFS_MP(mp)           ((struct packfs_mount *)vfs_fsprivate(mp))
#define PACKFS_VP_MOUNT(vp)     PACKFS_MP(vnode_mount(vp))
/* File numbers start at 2, the traditional number of a root directory. */
#define PACKFS_INO(index)       ((uint64_t)(index) + 2)

static inline uint32_t
packfs_vp_index(struct packfs_mount *pm, vnode_t vp)
{
	return (uint32_t)((struct packfs_inode *)vnode_fsnode(vp) - pm->pm_inodes);
}

int packfs_mountroot(mount_t mp, vnode_t devvp, vfs_context_t ctx);
int packfs_node_vnode(struct packfs_mount *pm, uint32_t index, vnode_t dvp, struct componentname *cnp, vnode_t *vpp);

extern int(**packfs_vnodeop_p)(void *);

#endif /* KERNEL */
#endif /* _MISCFS_PACKFS_H_ */
