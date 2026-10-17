/*
 * apfs vnode operations.  Read only; see apfs.h.
 *
 * Every call into apfs_lib is made under the mount's lock, because apfs_lib hands out
 * pointers into its own block cache and its cursors carry no locking.
 */

#include <kern/kalloc.h>
#include <libkern/OSAtomic.h>
#include <libkern/libkern.h>
#include <miscfs/apfs/apfs.h>
#include <sys/attr.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/namei.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/ubc.h>
#include <sys/uio.h>
#include <sys/mount_internal.h>
#include <sys/unistd.h>
#include <sys/vnode_internal.h>
#include <sys/xattr.h>
#include <vfs/vfs_support.h>

/* How much is read out of a file at a time when copying into a uio. */
#define APFS_IOBUF 65536

void
apfs_lock(struct apfs_mount *am)
{
	uint64_t t0 = apfs_ticks();

	lck_mtx_lock(&am->am_lock);
	OSAddAtomic64((int64_t)(apfs_ticks() - t0), (volatile int64_t *)&am->am_lock_wait_ticks);
	OSAddAtomic64(1, (volatile int64_t *)&am->am_lock_waits);
	am->am_lock_t0 = apfs_ticks();
}

void
apfs_unlock(struct apfs_mount *am)
{
	am->am_lock_held_ticks += apfs_ticks() - am->am_lock_t0;
	lck_mtx_unlock(&am->am_lock);
}

/* The inode record of the object a vnode stands for. */
static int
apfs_vp_inode(vnode_t vp, struct apfs_inode *ino)
{
	struct apfs_node *an = APFS_VP(vp);

	return apfs_inode_get(&an->an_mount->am_vol, an->an_id, ino);
}

/* ------------------------------------------------------------------ lookup */

static int
apfs_vnop_lookup(struct vnop_lookup_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct apfs_mount *am = APFS_VP_MOUNT(dvp);
	struct apfs_node *dn = APFS_VP(dvp);
	uint64_t id = 0;
	uint8_t dt = 0;
	int error;

	*ap->a_vpp = NULLVP;
	if (!vnode_isdir(dvp)) {
		return ENOTDIR;
	}
	/* read only: anything that would change the directory is refused up front */
	if (cnp->cn_nameiop != LOOKUP) {
		return EROFS;
	}
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		error = vnode_get(dvp);
		if (error == 0) {
			*ap->a_vpp = dvp;
		}
		return error;
	}
	if (cnp->cn_flags & ISDOTDOT) {
		return apfs_node_vnode(am, dn->an_parent, NULLVP, NULL, ap->a_vpp);
	}

	apfs_lock(am);
	error = apfs_lookup(&am->am_vol, dn->an_id, cnp->cn_nameptr,
	    (size_t)cnp->cn_namelen, &id, &dt);
	apfs_unlock(am);
	if (error) {
		return error;
	}
	return apfs_node_vnode(am, id, dvp, cnp, ap->a_vpp);
}

/* ------------------------------------------------------------------ attributes */

static int
apfs_vnop_getattr(struct vnop_getattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_inode ino;
	uint64_t size = 0;
	int error;

	ino = an->an_ino;
	size = an->an_size;

	VATTR_RETURN(vap, va_rdev, (dev_t)ino.rdev);
	VATTR_RETURN(vap, va_nlink, vnode_isdir(vp) ? 2 : (uint32_t)(ino.nchildren > 0
	    ? ino.nchildren : 1));
	VATTR_RETURN(vap, va_data_size, size);
	VATTR_RETURN(vap, va_total_size, size);
	VATTR_RETURN(vap, va_total_alloc, ino.alloced_size);
	VATTR_RETURN(vap, va_data_alloc, ino.alloced_size);
	VATTR_RETURN(vap, va_iosize, am->am_nx.block_size);
	VATTR_RETURN(vap, va_uid, ino.owner);
	VATTR_RETURN(vap, va_gid, ino.group);
	VATTR_RETURN(vap, va_mode, ino.mode & 07777);
	VATTR_RETURN(vap, va_flags, ino.bsd_flags);
	VATTR_RETURN(vap, va_fileid, an->an_id);
	VATTR_RETURN(vap, va_linkid, an->an_id);
	VATTR_RETURN(vap, va_parentid, ino.parent_id);
	VATTR_RETURN(vap, va_fsid, (uint32_t)vfs_statfs(am->am_mp)->f_fsid.val[0]);
	VATTR_RETURN(vap, va_filerev, 0);
	VATTR_RETURN(vap, va_gen, 0);
	VATTR_RETURN(vap, va_type, vnode_vtype(vp));

	/* the times are nanoseconds since the Unix epoch */
	if (VATTR_IS_ACTIVE(vap, va_access_time)) {
		vap->va_access_time.tv_sec = (long)(ino.access_time / 1000000000ULL);
		vap->va_access_time.tv_nsec = (long)(ino.access_time % 1000000000ULL);
		VATTR_SET_SUPPORTED(vap, va_access_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
		vap->va_modify_time.tv_sec = (long)(ino.mod_time / 1000000000ULL);
		vap->va_modify_time.tv_nsec = (long)(ino.mod_time % 1000000000ULL);
		VATTR_SET_SUPPORTED(vap, va_modify_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_change_time)) {
		vap->va_change_time.tv_sec = (long)(ino.change_time / 1000000000ULL);
		vap->va_change_time.tv_nsec = (long)(ino.change_time % 1000000000ULL);
		VATTR_SET_SUPPORTED(vap, va_change_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_create_time)) {
		vap->va_create_time.tv_sec = (long)(ino.create_time / 1000000000ULL);
		vap->va_create_time.tv_nsec = (long)(ino.create_time % 1000000000ULL);
		VATTR_SET_SUPPORTED(vap, va_create_time);
	}
	return 0;
}

/* ------------------------------------------------------------------ reading */

static int
apfs_vnop_read(struct vnop_read_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_inode ino;
	uint8_t *buf;
	uint64_t size = 0;
	uint64_t t0, read_bytes = 0;
	int error = 0;

	if (vnode_isdir(vp)) {
		return EISDIR;
	}
	if (!vnode_isreg(vp)) {
		return EINVAL;
	}
	if (uio_offset(uio) < 0) {
		return EINVAL;
	}
	ino = APFS_VP(vp)->an_ino;
	size = APFS_VP(vp)->an_size;
	buf = kalloc_data(APFS_IOBUF, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	t0 = apfs_ticks();
	while (uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < size) {
		size_t want = (size_t)MIN((user_ssize_t)APFS_IOBUF, uio_resid(uio));
		size_t got = 0;

		apfs_lock(am);
		error = apfs_read_file(&am->am_vol, &ino, &APFS_VP(vp)->an_cf,
		    (uint64_t)uio_offset(uio), buf, want, &got);
		apfs_unlock(am);
		if (error) {
			break;
		}
		if (got == 0) {
			break;
		}
		error = uiomove((caddr_t)buf, (int)got, uio);
		if (error) {
			break;
		}
		read_bytes += got;
	}
	kfree_data(buf, APFS_IOBUF);
	apfs_lock(am);
	am->am_read_ticks += apfs_ticks() - t0;
	am->am_read_calls++;
	am->am_read_bytes += read_bytes;
	apfs_unlock(am);
	return error;
}

/*
 * Page in.  Everything goes through apfs_read_file rather than the cluster layer: a
 * compressed file has no block mapping at all, and a plain one is read out of the extents
 * just as cheaply here.
 */
static int
apfs_vnop_pagein(struct vnop_pagein_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	upl_t upl = ap->a_pl;
	upl_offset_t upl_offset = ap->a_pl_offset;
	off_t offset = ap->a_f_offset;
	struct apfs_inode ino;
	vm_offset_t address = 0;
	uint64_t size = 0;
	uint64_t t0 = apfs_ticks();
	off_t at, end;
	int error = 0;

	if (upl == NULL) {
		return EINVAL;
	}
	if (offset < 0 || (offset & PAGE_MASK_64) || (ap->a_size & PAGE_MASK) ||
	    (upl_offset & PAGE_MASK)) {
		error = EINVAL;
	} else {
		ino = APFS_VP(vp)->an_ino;
		size = APFS_VP(vp)->an_size;
		if (ubc_upl_map(upl, &address) != KERN_SUCCESS) {
			error = ENOMEM;
		}
	}
	if (error) {
		if (!(ap->a_flags & UPL_NOCOMMIT)) {
			ubc_upl_abort_range(upl, upl_offset, (upl_size_t)ap->a_size,
			    UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_ERROR);
		}
		return error;
	}

	/*
	 * Say once how big a range the paging machinery asks for.  It decides how much the
	 * holding of a decompressed piece can save: if a request already covers a whole
	 * 64 KB piece, the piece was being decompressed once per request anyway.
	 */
	{
		static bool said;

		if (!said) {
			said = true;
			printf("apfs: pagein asks for %u bytes at a time (page size %u)\n",
			    (unsigned)ap->a_size, (unsigned)PAGE_SIZE);
		}
	}
	end = offset + (off_t)ap->a_size;
	for (at = offset; at < end;) {
		uint8_t *dst = (uint8_t *)address + upl_offset + (at - offset);
		size_t want, got = 0;

		if ((uint64_t)at >= size) {
			bzero(dst, (size_t)(end - at));         /* past the end of the file */
			break;
		}
		want = (size_t)MIN(end - at, (off_t)(size - (uint64_t)at));
		apfs_lock(am);
		error = apfs_read_file(&am->am_vol, &ino, &APFS_VP(vp)->an_cf, (uint64_t)at,
		    dst, want, &got);
		apfs_unlock(am);
		if (error) {
			break;
		}
		if (got == 0) {
			bzero(dst, (size_t)(end - at));
			break;
		}
		at += got;
		/* the last page of the file: zero what lies past its end */
		if ((uint64_t)at >= size && at < end) {
			bzero((uint8_t *)address + upl_offset + (at - offset),
			    (size_t)(end - at));
			break;
		}
	}

	ubc_upl_unmap(upl);
	apfs_lock(am);
	am->am_pagein_ticks += apfs_ticks() - t0;
	am->am_pagein_calls++;
	am->am_pagein_bytes += ap->a_size;
	apfs_unlock(am);
	if (!(ap->a_flags & UPL_NOCOMMIT)) {
		if (error) {
			ubc_upl_abort_range(upl, upl_offset, (upl_size_t)ap->a_size,
			    UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_ERROR);
		} else {
			ubc_upl_commit_range(upl, upl_offset, (upl_size_t)ap->a_size,
			    UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
	}
	return error;
}

/* ------------------------------------------------------------------ directories */

#define APFS_DIRSIZ(namlen) \
	((sizeof(struct dirent) - (MAXNAMLEN + 1)) + (((namlen) + 1 + 3) & ~3))

struct apfs_rd_ctx {
	struct uio *rd_uio;
	off_t       rd_start;           /* where this call is to resume */
	off_t       rd_pos;             /* where we are in the stream of entries */
	int         rd_count;
	bool        rd_full;
	int         rd_error;
};

static int
apfs_vnop_readdir_one(struct apfs_rd_ctx *rc, const char *name, size_t name_len,
    uint64_t file_id, uint8_t d_type)
{
	struct dirent de;

	if (name_len > MAXNAMLEN) {
		return 0;               /* cannot be named through this interface; skip it */
	}
	bzero(&de, sizeof(de));
	de.d_fileno = (ino_t)file_id;
	de.d_type = d_type;
	de.d_namlen = (uint8_t)name_len;
	de.d_reclen = (uint16_t)APFS_DIRSIZ(name_len);

	if (rc->rd_pos >= rc->rd_start) {
		if (uio_resid(rc->rd_uio) < (user_ssize_t)de.d_reclen) {
			rc->rd_full = true;
			return EEXIST;      /* stop the walk: no room for this one */
		}
		memcpy(de.d_name, name, name_len);
		rc->rd_error = uiomove((caddr_t)&de, de.d_reclen, rc->rd_uio);
		if (rc->rd_error) {
			return EEXIST;
		}
		rc->rd_count++;
	}
	rc->rd_pos += de.d_reclen;
	return 0;
}

static int
apfs_readdir_cb(void *ctx, const char *name, size_t name_len, uint64_t file_id,
    uint8_t d_type)
{
	return apfs_vnop_readdir_one(ctx, name, name_len, file_id, d_type);
}

static int
apfs_vnop_readdir(struct vnop_readdir_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_rd_ctx rc;
	int error;

	if (ap->a_flags & (VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF)) {
		return EINVAL;
	}
	if (!vnode_isdir(vp)) {
		return ENOTDIR;
	}
	bzero(&rc, sizeof(rc));
	rc.rd_uio = ap->a_uio;
	rc.rd_start = uio_offset(ap->a_uio);

	/*
	 * A directory's entries are a run of records in the tree with no index, so there is
	 * nothing to seek to: the walk starts at the beginning every time and skips what
	 * has already been handed over.  "." and ".." come first and are not in the tree.
	 */
	error = apfs_vnop_readdir_one(&rc, ".", 1, an->an_id, DT_DIR);
	if (error == 0) {
		error = apfs_vnop_readdir_one(&rc, "..", 2, an->an_parent, DT_DIR);
	}
	if (error == 0) {
		apfs_lock(am);
		error = apfs_readdir(&am->am_vol, an->an_id, apfs_readdir_cb, &rc);
		apfs_unlock(am);
	}
	if (error == EEXIST) {
		error = 0;              /* we stopped it ourselves */
	}
	if (rc.rd_error != 0) {
		return rc.rd_error;
	}
	if (error != 0) {
		return error;
	}
	uio_setoffset(ap->a_uio, rc.rd_pos);
	if (ap->a_eofflag != NULL) {
		*ap->a_eofflag = rc.rd_full ? 0 : 1;
	}
	if (ap->a_numdirent != NULL) {
		*ap->a_numdirent = rc.rd_count;
	}
	return 0;
}

static int
apfs_vnop_readlink(struct vnop_readlink_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_node *an = APFS_VP(vp);
	char *buf;
	size_t n = 0;
	int error;

	if (!vnode_islnk(vp)) {
		return EINVAL;
	}
	buf = kalloc_data(MAXPATHLEN, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	apfs_lock(am);
	error = apfs_readlink(&am->am_vol, an->an_id, buf, MAXPATHLEN, &n);
	apfs_unlock(am);
	if (error == 0) {
		error = uiomove(buf, (int)n, ap->a_uio);
	}
	kfree_data(buf, MAXPATHLEN);
	return error;
}

/* ------------------------------------------------------------------ attributes */

/*
 * The attributes the operating system keeps for itself are not shown to user space: they
 * are how a symbolic link and a compressed file are stored, not things a reader should
 * see.
 */
static bool
apfs_xattr_hidden(const char *name)
{
	return strcmp(name, APFS_SYMLINK_EA_NAME) == 0 ||
	       strcmp(name, APFS_DECMPFS_XATTR_NAME) == 0 ||
	       strcmp(name, APFS_RSRC_FORK_XATTR_NAME) == 0;
}

static int
apfs_vnop_getxattr(struct vnop_getxattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_xattr xa;
	uint8_t *buf;
	size_t n = 0, whole;
	int error;

	if (ap->a_name == NULL) {
		return EINVAL;
	}
	if (apfs_xattr_hidden(ap->a_name)) {
		return ENOATTR;
	}
	apfs_lock(am);
	error = apfs_xattr_lookup(&am->am_vol, an->an_id, ap->a_name, &xa);
	apfs_unlock(am);
	if (error) {
		return error == ENOENT ? ENOATTR : error;
	}
	whole = (xa.flags & APFS_XATTR_DATA_STREAM) ? (size_t)xa.stream_size : xa.data_len;
	if (ap->a_uio == NULL) {
		*ap->a_size = whole;    /* only the size was asked for */
		return 0;
	}
	if (uio_resid(ap->a_uio) < (user_ssize_t)whole) {
		return ERANGE;
	}
	buf = kalloc_data(whole ? whole : 1, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	apfs_lock(am);
	error = apfs_xattr_read(&am->am_vol, an->an_id, ap->a_name, buf, whole, &n);
	apfs_unlock(am);
	if (error == 0) {
		error = uiomove((caddr_t)buf, (int)n, ap->a_uio);
	}
	kfree_data(buf, whole ? whole : 1);
	return error;
}

struct apfs_lx_ctx {
	struct uio *lx_uio;
	size_t      lx_size;
	int         lx_error;
};

static int
apfs_listxattr_cb(void *ctx, const char *name, size_t name_len,
    __unused const struct apfs_xattr *xa)
{
	struct apfs_lx_ctx *lc = ctx;

	if (apfs_xattr_hidden(name)) {
		return 0;
	}
	if (lc->lx_uio == NULL) {
		lc->lx_size += name_len + 1;
		return 0;
	}
	if (uio_resid(lc->lx_uio) < (user_ssize_t)(name_len + 1)) {
		lc->lx_error = ERANGE;
		return EEXIST;
	}
	lc->lx_error = uiomove((caddr_t)(uintptr_t)name, (int)name_len + 1, lc->lx_uio);
	return lc->lx_error ? EEXIST : 0;
}

static int
apfs_vnop_listxattr(struct vnop_listxattr_args *ap)
{
	struct apfs_mount *am = APFS_VP_MOUNT(ap->a_vp);
	struct apfs_node *an = APFS_VP(ap->a_vp);
	struct apfs_lx_ctx lc;
	int error;

	bzero(&lc, sizeof(lc));
	lc.lx_uio = ap->a_uio;
	apfs_lock(am);
	error = apfs_xattr_list(&am->am_vol, an->an_id, apfs_listxattr_cb, &lc);
	apfs_unlock(am);
	if (error == EEXIST) {
		error = 0;
	}
	if (error == 0 && lc.lx_error != 0) {
		return lc.lx_error;
	}
	if (error) {
		return error;
	}
	if (ap->a_uio == NULL && ap->a_size != NULL) {
		*ap->a_size = lc.lx_size;
	}
	return 0;
}

/* ------------------------------------------------------------------ the rest */

static int
apfs_vnop_open(struct vnop_open_args *ap)
{
	if (ap->a_mode & (FWRITE | O_TRUNC | O_CREAT)) {
		return EROFS;
	}
	return 0;
}

static int
apfs_vnop_close(__unused struct vnop_close_args *ap)
{
	return 0;
}

static int
apfs_vnop_mmap(__unused struct vnop_mmap_args *ap)
{
	/* the mount is read-only, so VFS has already refused a writable mapping */
	return 0;
}

static int
apfs_vnop_mnomap(__unused struct vnop_mnomap_args *ap)
{
	return 0;
}

static int
apfs_vnop_inactive(__unused struct vnop_inactive_args *ap)
{
	return 0;
}

static int
apfs_vnop_reclaim(struct vnop_reclaim_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_node *an = APFS_VP(vp);

	if (an != NULL) {
		apfs_lock(an->an_mount);
		apfs_cfile_fini(&an->an_cf);
		apfs_unlock(an->an_mount);
		apfs_node_detach(an);
		vnode_clearfsnode(vp);
		kfree_type(struct apfs_node, an);
	}
	return 0;
}

static int
apfs_vnop_pathconf(struct vnop_pathconf_args *ap)
{
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = 32767;
		return 0;
	case _PC_NAME_MAX:
		*ap->a_retval = 255;
		return 0;
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX;
		return 0;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		return 0;
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
		return 0;
	case _PC_CASE_SENSITIVE:
		*ap->a_retval = (APFS_VP_MOUNT(ap->a_vp)->am_vol.incompat &
		    APFS_INCOMPAT_CASE_INSENSITIVE) ? 0 : 1;
		return 0;
	case _PC_CASE_PRESERVING:
		*ap->a_retval = 1;
		return 0;
	case _PC_XATTR_SIZE_BITS:
		*ap->a_retval = 32;
		return 0;
	default:
		return EINVAL;
	}
}

#define VOPFUNC int (*)(void *)

int(**apfs_vnodeop_p)(void *);
const struct vnodeopv_entry_desc apfs_vnodeop_entries[] = {
	{ .opve_op = &vnop_default_desc, .opve_impl = (VOPFUNC)vn_default_error },
	{ .opve_op = &vnop_lookup_desc, .opve_impl = (VOPFUNC)apfs_vnop_lookup },
	{ .opve_op = &vnop_open_desc, .opve_impl = (VOPFUNC)apfs_vnop_open },
	{ .opve_op = &vnop_close_desc, .opve_impl = (VOPFUNC)apfs_vnop_close },
	{ .opve_op = &vnop_getattr_desc, .opve_impl = (VOPFUNC)apfs_vnop_getattr },
	{ .opve_op = &vnop_read_desc, .opve_impl = (VOPFUNC)apfs_vnop_read },
	{ .opve_op = &vnop_getxattr_desc, .opve_impl = (VOPFUNC)apfs_vnop_getxattr },
	{ .opve_op = &vnop_listxattr_desc, .opve_impl = (VOPFUNC)apfs_vnop_listxattr },
	{ .opve_op = &vnop_fsync_desc, .opve_impl = (VOPFUNC)nop_fsync },
	{ .opve_op = &vnop_readdir_desc, .opve_impl = (VOPFUNC)apfs_vnop_readdir },
	{ .opve_op = &vnop_readlink_desc, .opve_impl = (VOPFUNC)apfs_vnop_readlink },
	{ .opve_op = &vnop_inactive_desc, .opve_impl = (VOPFUNC)apfs_vnop_inactive },
	{ .opve_op = &vnop_reclaim_desc, .opve_impl = (VOPFUNC)apfs_vnop_reclaim },
	{ .opve_op = &vnop_pathconf_desc, .opve_impl = (VOPFUNC)apfs_vnop_pathconf },
	{ .opve_op = &vnop_mmap_desc, .opve_impl = (VOPFUNC)apfs_vnop_mmap },
	{ .opve_op = &vnop_mnomap_desc, .opve_impl = (VOPFUNC)apfs_vnop_mnomap },
	{ .opve_op = &vnop_pagein_desc, .opve_impl = (VOPFUNC)apfs_vnop_pagein },
	{ .opve_op = (struct vnodeop_desc *)NULL, .opve_impl = (VOPFUNC)NULL }
};
const struct vnodeopv_desc apfs_vnodeop_opv_desc = {
	.opv_desc_vector_p = &apfs_vnodeop_p,
	.opv_desc_ops = apfs_vnodeop_entries
};
