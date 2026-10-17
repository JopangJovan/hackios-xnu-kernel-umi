/*
 * apfs mount-level operations.  See apfs.h; the format is in apfs_lib.c.
 */

#include <kern/assert.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <miscfs/apfs/apfs.h>
#include <sys/attr.h>
#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <uuid/uuid.h>
#include <sys/vnode_internal.h>
#include <sys/fcntl.h>

static LCK_GRP_DECLARE(apfs_lck_grp, "apfs");

/* Blocks are read from the device in pieces of this size. */
#define APFS_META_CHUNK 4096

/*
 * The device, as apfs_lib wants it.  Offsets and lengths are whole container blocks, and
 * a container block is a multiple of the device's block size (both are powers of two and
 * the container's is at least 4096), so every read lines up.
 */
struct apfs_devctx {
	vnode_t  dc_devvp;
	uint32_t dc_devblock;
};

static int
apfs_dev_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct apfs_devctx *dc = ctx;
	char *out = buf;

	if ((off % dc->dc_devblock) != 0) {
		return EINVAL;
	}
	while (len > 0) {
		buf_t bp = NULL;
		size_t chunk = len < APFS_META_CHUNK ? len : APFS_META_CHUNK;
		int error = (int)buf_meta_bread(dc->dc_devvp,
		    (daddr64_t)(off / dc->dc_devblock), APFS_META_CHUNK, NOCRED, &bp);

		if (error) {
			if (bp != NULL) {
				buf_brelse(bp);
			}
			return error;
		}
		memcpy(out, (const void *)buf_dataptr(bp), chunk);
		/*
		 * apfs_lib keeps its own cache of what it needs, and a volume this size would
		 * otherwise fill the buffer cache with blocks nobody asks for twice.
		 */
		buf_markinvalid(bp);
		buf_brelse(bp);

		out += chunk;
		off += chunk;
		len -= chunk;
	}
	return 0;
}

/*
 * A file's contents, straight into the caller's buffer.  One buffer of our own per
 * request, outside the buffer cache: these bytes are usually wanted once, and putting a
 * 64 KB read through the cache a block at a time meant sixteen requests to the device and
 * sixteen cached blocks nobody would ask for again.
 */
#define APFS_BULK_MAX (64 * 1024)

static int
apfs_dev_read_bulk(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct apfs_devctx *dc = ctx;
	char *out = buf;

	if ((off % dc->dc_devblock) != 0 || (len % dc->dc_devblock) != 0) {
		return EINVAL;
	}
	while (len > 0) {
		size_t n = len < APFS_BULK_MAX ? len : APFS_BULK_MAX;
		buf_t bp = buf_alloc(dc->dc_devvp);
		int error;

		if (bp == NULL) {
			return ENOMEM;
		}
		buf_reset(bp, B_READ);
		buf_setblkno(bp, (daddr64_t)(off / dc->dc_devblock));
		buf_setlblkno(bp, (daddr64_t)(off / dc->dc_devblock));
		buf_setcount(bp, (uint32_t)n);
		buf_setdataptr(bp, (uintptr_t)out);
		error = VNOP_STRATEGY(bp);
		if (error == 0) {
			error = (int)buf_biowait(bp);
		}
		buf_free(bp);
		if (error) {
			return error;
		}
		out += n;
		off += n;
		len -= n;
	}
	return 0;
}

/* ------------------------------------------------------------------ vnodes */

static uint32_t
apfs_hash_index(uint64_t obj_id)
{
	/* the low bits of an identifier are the ones that differ between neighbours */
	return (uint32_t)((obj_id ^ (obj_id >> 17)) % APFS_HASH_BUCKETS);
}

/* The vnode for an object, making one if there is not one already. */
int
apfs_node_vnode(struct apfs_mount *am, uint64_t obj_id, vnode_t dvp,
    struct componentname *cnp, vnode_t *vpp)
{
	struct vnode_fsparam vfsp;
	struct apfs_node *an;
	struct apfs_inode ino;
	uint32_t b = apfs_hash_index(obj_id);
	uint64_t size = 0;
	enum vtype vtype;
	int error;

	/* already have one? */
	lck_mtx_lock(&am->am_lock);
	for (an = am->am_hash[b]; an != NULL; an = an->an_next) {
		if (an->an_id == obj_id && an->an_attached) {
			vnode_t vp = an->an_vp;
			uint32_t vid = vnode_vid(vp);

			lck_mtx_unlock(&am->am_lock);
			if (vnode_getwithvid(vp, vid) == 0) {
				*vpp = vp;
				return 0;
			}
			/* it was going away under us: fall through and make a new one */
			lck_mtx_lock(&am->am_lock);
			break;
		}
	}

	error = apfs_inode_get(&am->am_vol, obj_id, &ino);
	if (error == 0) {
		error = apfs_inode_size(&am->am_vol, &ino, &size);
		if (error) {
			size = ino.size;       /* the stream's size is still better than nothing */
			error = 0;
		}
	}
	lck_mtx_unlock(&am->am_lock);
	if (error) {
		return error;
	}

	switch (ino.mode & APFS_S_IFMT) {
	case APFS_S_IFREG:  vtype = VREG; break;
	case APFS_S_IFDIR:  vtype = VDIR; break;
	case APFS_S_IFLNK:  vtype = VLNK; break;
	case APFS_S_IFCHR:  vtype = VCHR; break;
	case APFS_S_IFBLK:  vtype = VBLK; break;
	case APFS_S_IFIFO:  vtype = VFIFO; break;
	case APFS_S_IFSOCK: vtype = VSOCK; break;
	default:
		return EIO;
	}

	an = kalloc_type(struct apfs_node, Z_WAITOK | Z_ZERO);
	if (an == NULL) {
		return ENOMEM;
	}
	an->an_mount = am;
	an->an_id = obj_id;
	an->an_parent = ino.parent_id;
	an->an_mode = ino.mode;
	an->an_size = size;
	an->an_ino = ino;       /* nothing here can change: the mount is read only */

	bzero(&vfsp, sizeof(vfsp));
	vfsp.vnfs_mp = am->am_mp;
	vfsp.vnfs_vtype = vtype;
	vfsp.vnfs_str = "apfs";
	vfsp.vnfs_dvp = dvp;
	vfsp.vnfs_fsnode = an;
	vfsp.vnfs_vops = apfs_vnodeop_p;
	vfsp.vnfs_markroot = (obj_id == am->am_root_id);
	vfsp.vnfs_marksystem = 0;
	vfsp.vnfs_rdev = (vtype == VCHR || vtype == VBLK) ? (dev_t)ino.rdev : 0;
	vfsp.vnfs_filesize = size;
	vfsp.vnfs_cnp = cnp;
	/* let VFS cache the name only when it came from a lookup that has one */
	vfsp.vnfs_flags = (dvp != NULL && cnp != NULL) ? 0 : VNFS_NOCACHE;

	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vfsp, &an->an_vp);
	if (error) {
		kfree_type(struct apfs_node, an);
		return error;
	}
	vnode_settag(an->an_vp, VT_OTHER);

	lck_mtx_lock(&am->am_lock);
	an->an_next = am->am_hash[b];
	am->am_hash[b] = an;
	an->an_attached = true;
	lck_mtx_unlock(&am->am_lock);

	*vpp = an->an_vp;
	return 0;
}

/* Take a node out of the hash; called from vnop_reclaim. */
void
apfs_node_detach(struct apfs_node *an)
{
	struct apfs_mount *am = an->an_mount;
	uint32_t b = apfs_hash_index(an->an_id);
	struct apfs_node **pp;

	lck_mtx_lock(&am->am_lock);
	for (pp = &am->am_hash[b]; *pp != NULL; pp = &(*pp)->an_next) {
		if (*pp == an) {
			*pp = an->an_next;
			break;
		}
	}
	an->an_attached = false;
	lck_mtx_unlock(&am->am_lock);
}

/* ------------------------------------------------------------------ mount */

static void
apfs_free_mount(struct apfs_mount *am)
{
	apfs_nx_close(&am->am_nx);
	lck_mtx_destroy(&am->am_lock, &apfs_lck_grp);
	kfree_type(struct apfs_mount, am);
}

static int
apfs_mount_common(mount_t mp, vnode_t devvp, vfs_context_t ctx)
{
	struct apfs_mount *am;
	struct apfs_devctx *dc;
	struct apfs_dev dev;
	struct vfsstatfs *sp;
	uint32_t devblock = 0;
	uint64_t devblocks = 0;
	int error;

	if (VNOP_IOCTL(devvp, DKIOCGETBLOCKSIZE, (caddr_t)&devblock, 0, ctx) != 0 ||
	    devblock == 0 || (APFS_META_CHUNK % devblock) != 0) {
		return EINVAL;
	}
	(void)VNOP_IOCTL(devvp, DKIOCGETBLOCKCOUNT, (caddr_t)&devblocks, 0, ctx);

	am = kalloc_type(struct apfs_mount, Z_WAITOK | Z_ZERO);
	if (am == NULL) {
		return ENOMEM;
	}
	lck_mtx_init(&am->am_lock, &apfs_lck_grp, LCK_ATTR_NULL);
	am->am_mp = mp;
	am->am_devvp = devvp;
	am->am_devblock = devblock;

	dc = kalloc_type(struct apfs_devctx, Z_WAITOK | Z_ZERO);
	if (dc == NULL) {
		apfs_free_mount(am);
		return ENOMEM;
	}
	dc->dc_devvp = devvp;
	dc->dc_devblock = devblock;

	bzero(&dev, sizeof(dev));
	dev.ctx = dc;
	dev.size = devblocks * (uint64_t)devblock;
	dev.read = apfs_dev_read;
	dev.read_bulk = apfs_dev_read_bulk;

	error = apfs_nx_open(&am->am_nx, &dev);
	if (error) {
		/*
		 * EINVAL means the magic was not there, so this is not an APFS container at
		 * all: as the root filesystem that lets vfs_mountroot() try the next one
		 * without a word.
		 */
		if (error != EINVAL) {
			printf("apfs: cannot read the container: %d\n", error);
		}
		kfree_type(struct apfs_devctx, dc);
		apfs_free_mount(am);
		return error;
	}

	/* the first volume that has a root directory */
	{
		uint32_t i;
		bool got = false;

		for (i = 0; i < APFS_NX_MAX_FILE_SYSTEMS && !got; i++) {
			struct apfs_inode ino;

			if (am->am_nx.fs_oid[i] == 0) {
				continue;
			}
			if (apfs_vol_open(&am->am_nx, i, &am->am_vol) != 0) {
				continue;
			}
			if (apfs_inode_get(&am->am_vol, APFS_ROOT_DIR_INO_NUM, &ino) == 0) {
				got = true;
			}
		}
		if (!got) {
			printf("apfs: no readable volume with a root directory\n");
			kfree_type(struct apfs_devctx, dc);
			apfs_free_mount(am);
			return EINVAL;
		}
	}
	am->am_root_id = APFS_ROOT_DIR_INO_NUM;

	vfs_setfsprivate(mp, am);
	vfs_setflags(mp, MNT_RDONLY | MNT_LOCAL);
	vfs_setlocklocal(mp);

	sp = vfs_statfs(mp);
	sp->f_bsize = am->am_nx.block_size;
	sp->f_iosize = am->am_nx.block_size;
	sp->f_blocks = am->am_nx.block_count;
	sp->f_bfree = 0;
	sp->f_bavail = 0;
	sp->f_bused = am->am_vol.alloc_count;
	sp->f_files = am->am_vol.num_files + am->am_vol.num_dirs;
	sp->f_ffree = 0;
	sp->f_fsid.val[0] = (int32_t)vnode_specrdev(devvp);
	sp->f_fsid.val[1] = vfs_typenum(mp);
	strlcpy(sp->f_fstypename, "apfs", sizeof(sp->f_fstypename));

	printf("apfs: volume \"%s\": %llu files, %llu directories, %llu blocks of %u used\n",
	    am->am_vol.name, (unsigned long long)am->am_vol.num_files,
	    (unsigned long long)am->am_vol.num_dirs,
	    (unsigned long long)am->am_vol.alloc_count, am->am_nx.block_size);
	return 0;
}

/* As the root filesystem: EINVAL lets vfs_mountroot() try the next quietly. */
int
apfs_mountroot(mount_t mp, vnode_t devvp, vfs_context_t ctx)
{
	return apfs_mount_common(mp, devvp, ctx);
}

/*
 * mount(2), and kernel_mount().  The mount data is the path of the block device holding
 * the container, as a string.  VFS does not resolve it for us the way it does for a
 * filesystem marked VFC_VFSLOCALARGS -- see the note in vfs_conf.c for why this one is
 * not -- so it is looked up and opened here, and closed again in apfs_unmount().
 */
static int
apfs_mount(mount_t mp, vnode_t devvp, user_addr_t data, vfs_context_t ctx)
{
	char *path;
	vnode_t vp = NULLVP;
	size_t len = 0;
	int error;

	if (vfs_isupdate(mp)) {
		/* nothing about a mounted volume can change, least of all to read-write */
		return vfs_iswriteupgrade(mp) ? EROFS : 0;
	}
	if (devvp != NULLVP) {
		/* mounted as the root: VFS found the device and owns the vnode */
		return apfs_mount_common(mp, devvp, ctx);
	}
	if (data == USER_ADDR_NULL) {
		return EINVAL;
	}
	path = kalloc_data(MAXPATHLEN, Z_WAITOK);
	if (path == NULL) {
		return ENOMEM;
	}
	if (vfs_iskernelmount(mp)) {
		/* kernel_mount() hands the pointer through untouched */
		error = (strlcpy(path, CAST_DOWN(char *, data), MAXPATHLEN) < MAXPATHLEN)
		    ? 0 : ENAMETOOLONG;
	} else {
		error = copyinstr(data, path, MAXPATHLEN, &len);
	}
	if (error == 0) {
		error = vnode_lookup(path, 0, &vp, ctx);
	}
	if (error == 0 && !vnode_isblk(vp)) {
		error = ENOTBLK;
	}
	if (error == 0) {
		error = VNOP_OPEN(vp, FREAD, ctx);
		if (error == 0) {
			/*
			 * The iocount from the lookup is kept for as long as the volume is
			 * mounted; apfs_unmount() closes and drops it.
			 */
			error = apfs_mount_common(mp, vp, ctx);
			if (error == 0) {
				APFS_MP(mp)->am_owns_devvp = true;
			} else {
				(void)VNOP_CLOSE(vp, FREAD, ctx);
			}
		}
		if (error != 0) {
			vnode_put(vp);
		}
	} else if (vp != NULLVP) {
		vnode_put(vp);
	}
	kfree_data(path, MAXPATHLEN);
	return error;
}

static int
apfs_unmount(mount_t mp, int mntflags, vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_MP(mp);
	int error = vflush(mp, NULLVP, (mntflags & MNT_FORCE) ? FORCECLOSE : 0);

	if (error) {
		return error;
	}
	printf("apfs: %llu blocks read for metadata (%llu cache hits), %llu requests for "
	    "file data carrying %llu bytes\n",
	    (unsigned long long)am->am_nx.reads, (unsigned long long)am->am_nx.hits,
	    (unsigned long long)am->am_nx.bulk_calls,
	    (unsigned long long)am->am_nx.bulk_bytes);
	vfs_setfsprivate(mp, NULL);
	if (am->am_owns_devvp && am->am_devvp != NULLVP) {
		(void)VNOP_CLOSE(am->am_devvp, FREAD, ctx);
		vnode_put(am->am_devvp);
	}
	apfs_free_mount(am);
	return 0;
}

static int
apfs_root(mount_t mp, vnode_t *vpp, __unused vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_MP(mp);

	return apfs_node_vnode(am, am->am_root_id, NULLVP, NULL, vpp);
}

static int
apfs_vget(mount_t mp, ino64_t ino, vnode_t *vpp, __unused vfs_context_t ctx)
{
	return apfs_node_vnode(APFS_MP(mp), (uint64_t)ino, NULLVP, NULL, vpp);
}

static int
apfs_vfs_getattr(mount_t mp, struct vfs_attr *fsap, __unused vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_MP(mp);
	uint64_t files = am->am_vol.num_files + am->am_vol.num_dirs;

	VFSATTR_RETURN(fsap, f_objcount, files);
	VFSATTR_RETURN(fsap, f_bsize, am->am_nx.block_size);
	VFSATTR_RETURN(fsap, f_iosize, am->am_nx.block_size);
	VFSATTR_RETURN(fsap, f_blocks, am->am_nx.block_count);
	VFSATTR_RETURN(fsap, f_bfree, 0);
	VFSATTR_RETURN(fsap, f_bavail, 0);
	VFSATTR_RETURN(fsap, f_bused, am->am_vol.alloc_count);
	VFSATTR_RETURN(fsap, f_files, files);
	VFSATTR_RETURN(fsap, f_ffree, 0);

	if (VFSATTR_IS_ACTIVE(fsap, f_capabilities)) {
		vol_capabilities_attr_t *cap = &fsap->f_capabilities;
		bool nocase = (am->am_vol.incompat & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;

		bzero(cap, sizeof(*cap));
		/*
		 * An object's identifier never changes and apfs_vget() finds an object by it,
		 * which is what lets fsgetpath(2) and /.vol work for this volume -- see the
		 * note in packfs about what breaks in user space without it.
		 */
		cap->capabilities[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    VOL_CAP_FMT_PATH_FROM_ID |
		    (nocase ? 0 : VOL_CAP_FMT_CASE_SENSITIVE);
		cap->capabilities[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_ATTR;
		cap->valid[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_JOURNAL |
		    VOL_CAP_FMT_JOURNAL_ACTIVE |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_ZERO_RUNS |
		    VOL_CAP_FMT_CASE_SENSITIVE |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    VOL_CAP_FMT_HIDDEN_FILES |
		    VOL_CAP_FMT_PATH_FROM_ID;
		cap->valid[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_ATTR;
		VFSATTR_SET_SUPPORTED(fsap, f_capabilities);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {
		strlcpy(fsap->f_vol_name, am->am_vol.name, MAXPATHLEN);
		VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_uuid)) {
		memcpy(fsap->f_uuid, am->am_nx.uuid, sizeof(fsap->f_uuid));
		VFSATTR_SET_SUPPORTED(fsap, f_uuid);
	}
	return 0;
}

/* Nothing is ever dirty. */
static int
apfs_sync(__unused mount_t mp, __unused int waitfor, __unused vfs_context_t ctx)
{
	return 0;
}

static int
apfs_init(__unused struct vfsconf *vfsc)
{
	return 0;
}

struct vfsops apfs_vfsops = {
	.vfs_mount = apfs_mount,
	.vfs_unmount = apfs_unmount,
	.vfs_root = apfs_root,
	.vfs_getattr = apfs_vfs_getattr,
	.vfs_sync = apfs_sync,
	.vfs_vget = apfs_vget,
	.vfs_init = apfs_init,
};
