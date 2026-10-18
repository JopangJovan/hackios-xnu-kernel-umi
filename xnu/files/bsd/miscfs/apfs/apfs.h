/*
 * apfs: reading an Apple File System volume from the kernel.
 *
 * The format itself is in apfs_lib.{h,c}, which knows nothing of vnodes and is checked
 * against real images on the build machine (tools/apfs/run-tests.sh).  This is the layer
 * between that and VFS: a mount, a vnode per object, and the vnode operations.
 *
 * Read only.  Nothing here writes to the device.
 *
 * apfs_lib keeps a cache of metadata blocks and hands out pointers into it, and its
 * cursors are plain structures with no locking of their own, so every call into it is
 * made with am_lock held.  That makes the whole volume one lock wide, which is enough for
 * a read-only root: the work under the lock is a few block reads, usually from the cache.
 */

#ifndef _MISCFS_APFS_APFS_H_
#define _MISCFS_APFS_APFS_H_

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <kern/locks.h>

#include "apfs_lib.h"

/* Buckets in the table that finds an existing vnode by object identifier. */
#define APFS_HASH_BUCKETS 1024

struct apfs_node;

/*
 * What one round of vnop_read reads.  Alone on the volume, am_iobuf_size (boot-arg
 * apfs_round): a read(2) of that much is one request to apfs_lib, whose whole compressed
 * pieces are decompressed on several processors -- the bigger the round, the more there is
 * to share out.  With other reads in flight, APFS_ROUND_SHARED: the processors are busy
 * anyway, and a round that stays in the cache until uiomove copies it out is worth more.
 * One reader with 1 MB reads over the restore ramdisk, rounds of 128 KB / 256 KB / 512 KB /
 * 1 MB: 867 / 663 / 612 / 561 ms; four readers: 1429 / 1534 / 1747 / 2009 ms, and 1327
 * with rounds of 64 or 128 KB and no helpers.
 */
#define APFS_IOBUF_DEFAULT (1024 * 1024)
#define APFS_IOBUF_MAX     (2 * 1024 * 1024)
#define APFS_ROUND_SHARED  (128 * 1024)
#define APFS_IOBUF_KEEP    8

#define APFS_SYNTH_MAX     64
#define APFS_SYNTH_NAMELEN 96
/*
 * Object ids from here up are synthetic: the last few of the 60 bits an id has.  (Not
 * 0x0FFFFFFF00000000: the iOS root volume numbers its own objects from there.)
 */
#define APFS_SYNTH_BASE    0x0FFFFFFFFFFFF000ULL

struct apfs_synth {
	uint64_t parent;                /* the directory it is in (a real one, or synthetic) */
	uint64_t id;                    /* its own, or the directory it is another name for */
	bool     alias;                 /* another name for a directory of the volume */
	char     name[APFS_SYNTH_NAMELEN];
};

struct apfs_mount {
	mount_t            am_mp;
	vnode_t            am_devvp;
	lck_mtx_t          am_lock;
	struct apfs_nx     am_nx;
	struct apfs_vol    am_vol;
	uint32_t           am_devblock;
	bool               am_owns_devvp;  /* we opened it, so we close it */
	struct apfs_node  *am_hash[APFS_HASH_BUCKETS];
	uint64_t           am_root_id;
	/*
	 * Where the time goes at the vnode operations, to set beside the container's own
	 * counters (waiting for the device, decompressing).  Ticks; see apfs_ticks().
	 */
	uint64_t           am_pagein_calls, am_pagein_ticks, am_pagein_bytes;
	uint64_t           am_read_calls, am_read_ticks, am_read_bytes;
	/*
	 * What the one lock round apfs_lib costs: how long callers stood waiting for it, and
	 * how long it was held.  Held time is the part of the work that cannot happen on two
	 * processors at once, so with several readers it is the ceiling.
	 */
	uint64_t           am_lock_waits;
	uint64_t           am_lock_wait_ticks;
	uint64_t           am_lock_held_ticks;
	uint64_t           am_lock_t0;      /* when the holder took it; only the holder writes */
	/*
	 * vnop_read's buffers, kept for the next read rather than allocated and freed for
	 * every one (at that size the kernel goes to the VM).  Under am_lock.
	 */
	void              *am_iobuf[APFS_IOBUF_KEEP];
	uint32_t           am_iobuf_kept;
	uint32_t           am_iobuf_size;
	/*
	 * Synthetic directories: empty directories that are not on the volume, for mount
	 * points on a volume nobody can add a directory to -- what macOS's synthetic.conf is
	 * for its sealed system volume.  Added through sysctl kern.apfs_synthetic; seen by
	 * lookup and readdir of their parent, empty, owned by root.  Or another name for a
	 * directory of the same volume ("PATH=TARGET"): the directory itself, with PATH's
	 * parent for its "..".  Under am_lock.
	 */
	struct apfs_synth *am_synth;        /* APFS_SYNTH_MAX of them, made at the first */
	uint32_t           am_nsynth;
};

/* The private part of a vnode: which object it is. */
struct apfs_node {
	struct apfs_node  *an_next;     /* the hash chain */
	struct apfs_mount *an_mount;
	vnode_t            an_vp;
	uint64_t           an_id;       /* the object identifier */
	uint64_t           an_parent;
	uint16_t           an_mode;
	uint64_t           an_size;     /* the logical size, as a reader of the file sees it */
	bool               an_attached; /* an_vp is set and the node is in the hash */
	/*
	 * What has been worked out about how this file's contents are stored, kept for as
	 * long as the vnode lives: the table of compressed pieces and the piece last
	 * decompressed.  Without it every read and every page fault would find them again.
	 * Guarded by am_lock, like everything else that reaches apfs_lib.
	 */
	struct apfs_cfile  an_cf;
	/*
	 * The inode record as it was when the vnode was made.  Nothing on this volume can
	 * change -- the mount is read only and there is no writer anywhere -- so there is
	 * nothing to invalidate, and a read no longer walks the tree for the record and the
	 * decmpfs header on every call.
	 */
	struct apfs_inode  an_ino;
};

#define APFS_MP(mp)       ((struct apfs_mount *)vfs_fsprivate(mp))
const struct apfs_synth *apfs_synth_find(struct apfs_mount *am, uint64_t parent,
    const char *name, size_t len);      /* with am_lock held */

#define APFS_VP(vp)       ((struct apfs_node *)vnode_fsnode(vp))
#define APFS_VP_MOUNT(vp) (APFS_VP(vp)->an_mount)

/* apfs_vfsops.c */
extern struct vfsops apfs_vfsops;
int apfs_mountroot(mount_t mp, vnode_t devvp, vfs_context_t ctx);
int apfs_node_vnode(struct apfs_mount *am, uint64_t obj_id, vnode_t dvp,
    struct componentname *cnp, vnode_t *vpp);
void apfs_node_detach(struct apfs_node *an);

/* the breakdown of where the time went, straight to the log (apfs_vfsops.c) */
void apfs_stats_print(void);

void apfs_lock(struct apfs_mount *am);
void apfs_unlock(struct apfs_mount *am);

/* apfs_vnops.c */
extern const struct vnodeopv_desc apfs_vnodeop_opv_desc;
extern int (**apfs_vnodeop_p)(void *);

#endif /* _MISCFS_APFS_APFS_H_ */
