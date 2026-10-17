/*
 * Mount the APFS volume on /dev/rvblk0 and walk it through VFS, so that what the driver
 * does can be compared with what tools/apfs/apfs.py reads from the same image on the
 * build machine.  Only with boot-arg apfs_check.
 *
 *   apfs_check=1              walk the whole volume and print only the totals
 *   apfs_check=2              also print a line per entry
 *   apfs_check=3              mount it and stop, leaving it for user space to read
 *                             (userland/src/apfs_mmap_test.c, which is the only way the
 *                             driver's vnop_pagein ever runs)
 *   apfs_subtree=/usr         walk only that part of the volume
 *
 * A line is the same one tools/apfs/apfs-tool's census and `verify.py clines` print:
 *
 *   path <TAB> d_type <TAB> mode <TAB> uid <TAB> gid <TAB> size <TAB> hash-of-contents
 *
 * The whole volume is 318098 entries, and printing them all through the kernel log would
 * be 47 MB down a serial line, so by default each line is folded into a sum instead: the
 * lines are hashed one at a time and the hashes added together.  Addition does not care
 * what order the entries came in, which matters because this walk and the one on the host
 * need not visit a directory's entries in the same order.  The host adds up the same
 * hashes over its own lines; if the two sums and counts agree, every path, kind, mode,
 * owner, group, size and byte of content agreed.  When they do not, apfs_check=2 over a
 * subtree gives lines to diff.
 *
 * Everything here goes through VFS -- vnode_lookup, vnode_getattr, VNOP_READDIR,
 * VNOP_READLINK, vn_rdwr -- so it exercises the vnode operations and not just the format
 * reading underneath them.
 */

#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <sys/mount_internal.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/uio_internal.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/vnode_internal.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_apfs_check.h>

#define CHECK_MNT    "/private/var/apfs-check"
#define CHECK_PARENT "/private/var"
#define CHECK_LEAF   "apfs-check"
#define CHECK_DEV    "/dev/rvblk0"

#define FNV_INIT  0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

#define READ_CHUNK (64 * 1024)
#define DIR_CHUNK  4096
#define MAX_DEPTH  48
#define PATH_CAP   2048

static uint64_t
fnv1a(const uint8_t *p, size_t n, uint64_t h)
{
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= FNV_PRIME;
	}
	return h;
}

struct census {
	vfs_context_t ctx;
	char     *path;                 /* the whole path, starting with CHECK_MNT */
	size_t    rel;                  /* where the part inside the volume starts */
	size_t    len;
	uint8_t  *buf;                  /* for reading file contents */
	uint64_t  sum;                  /* the per-line hashes, added up */
	uint64_t  entries, dirs, files, links, others, bytes;
	uint32_t  errors;
	bool      print_lines;
	int       depth;
};

/* The format bits of a mode, which vnode_getattr does not report in va_mode. */
static uint16_t
ifmt_of(enum vtype t)
{
	switch (t) {
	case VREG:  return S_IFREG;
	case VDIR:  return S_IFDIR;
	case VLNK:  return S_IFLNK;
	case VCHR:  return S_IFCHR;
	case VBLK:  return S_IFBLK;
	case VFIFO: return S_IFIFO;
	case VSOCK: return S_IFSOCK;
	default:    return 0;
	}
}

/* FNV-1a over a regular file's contents, read through VNOP_READ. */
static int
hash_file(struct census *cs, vnode_t vp, uint64_t size, uint64_t *out)
{
	uint64_t h = FNV_INIT, off = 0;

	while (off < size) {
		int want = (int)((size - off > READ_CHUNK) ? READ_CHUNK : (size - off));
		int resid = 0;
		int error = vn_rdwr(UIO_READ, vp, (caddr_t)cs->buf, want, (off_t)off,
		    UIO_SYSSPACE, IO_NOCACHE, vfs_context_ucred(cs->ctx), &resid,
		    vfs_context_proc(cs->ctx));

		if (error) {
			return error;
		}
		if (resid == want) {
			break;                  /* nothing more to be had */
		}
		h = fnv1a(cs->buf, (size_t)(want - resid), h);
		cs->bytes += (uint64_t)(want - resid);
		off += (uint64_t)(want - resid);
	}
	*out = h;
	return 0;
}

/* FNV-1a over a symbolic link's target, read through VNOP_READLINK. */
static int
hash_link(struct census *cs, vnode_t vp, uint64_t *out)
{
	uio_t uio;
	char uio_buf[UIO_SIZEOF(1)];
	int error;
	size_t n;

	uio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_READ, uio_buf, sizeof(uio_buf));
	if (uio == NULL) {
		return ENOMEM;
	}
	uio_addiov(uio, CAST_USER_ADDR_T(cs->buf), MAXPATHLEN);
	error = VNOP_READLINK(vp, uio, cs->ctx);
	if (error) {
		return error;
	}
	n = MAXPATHLEN - (size_t)uio_resid(uio);
	cs->bytes += n;
	*out = fnv1a(cs->buf, n, FNV_INIT);
	return 0;
}

static void
census_entry(struct census *cs, uint8_t d_type)
{
	struct vnode_attr va;
	vnode_t vp = NULLVP;
	uint64_t h = 0, size = 0;
	uint16_t mode = 0;
	char line[PATH_CAP + 128];
	int error;
	int n;

	/*
	 * Do not follow a symbolic link: the point is to look at the link itself, as the host
	 * reader does.  Following them would report the target's kind, size and contents
	 * instead, and a link whose target is missing -- of which this volume has several --
	 * would look like a lookup failure.  Crossing mount points has to stay allowed: the
	 * path leads from the ramfs that holds the mount point into the volume itself.
	 */
	error = vnode_lookup(cs->path, VNODE_LOOKUP_NOFOLLOW, &vp, cs->ctx);
	if (error) {
		printf("apfs-check: %s: lookup failed: %d\n", cs->path + cs->rel, error);
		cs->errors++;
		return;
	}
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_mode);
	VATTR_WANTED(&va, va_uid);
	VATTR_WANTED(&va, va_gid);
	VATTR_WANTED(&va, va_data_size);
	error = vnode_getattr(vp, &va, cs->ctx);
	if (error) {
		printf("apfs-check: %s: getattr failed: %d\n", cs->path + cs->rel, error);
		cs->errors++;
		goto out;
	}
	mode = (uint16_t)(ifmt_of(vnode_vtype(vp)) | (VATTR_IS_SUPPORTED(&va, va_mode)
	    ? (va.va_mode & 07777) : 0));
	size = VATTR_IS_SUPPORTED(&va, va_data_size) ? va.va_data_size : 0;

	if (vnode_vtype(vp) == VREG) {
		cs->files++;
		error = hash_file(cs, vp, size, &h);
	} else if (vnode_vtype(vp) == VLNK) {
		cs->links++;
		error = hash_link(cs, vp, &h);
	} else if (vnode_vtype(vp) == VDIR) {
		cs->dirs++;
	} else {
		cs->others++;
	}
	if (error) {
		printf("apfs-check: %s: reading it failed: %d\n", cs->path + cs->rel, error);
		cs->errors++;
		goto out;
	}

	n = scnprintf(line, sizeof(line), "%s\t%u\t%06o\t%u\t%u\t%llu\t%016llx\n",
	    cs->path + cs->rel, d_type, mode,
	    VATTR_IS_SUPPORTED(&va, va_uid) ? va.va_uid : 0,
	    VATTR_IS_SUPPORTED(&va, va_gid) ? va.va_gid : 0,
	    (unsigned long long)size, (unsigned long long)h);
	cs->sum += fnv1a((const uint8_t *)line, (size_t)n, FNV_INIT);
	cs->entries++;
	if (cs->print_lines) {
		printf("apfs-line: %s", line);
	}
	if ((cs->entries % 20000) == 0) {
		printf("apfs-check: %llu entries so far\n", cs->entries);
	}
out:
	vnode_put(vp);
}

static void census_dir(struct census *cs);

/* Add "/name" to the path, do the entry, and put the path back as it was. */
static void
census_child(struct census *cs, const char *name, size_t name_len, uint8_t d_type)
{
	size_t saved = cs->len;

	if (cs->len + name_len + 2 > PATH_CAP) {
		printf("apfs-check: path below %s is too long\n", cs->path + cs->rel);
		cs->errors++;
		return;
	}
	cs->path[cs->len++] = '/';
	memcpy(cs->path + cs->len, name, name_len);
	cs->len += name_len;
	cs->path[cs->len] = '\0';

	census_entry(cs, d_type);
	if (d_type == DT_DIR && cs->depth < MAX_DEPTH) {
		cs->depth++;
		census_dir(cs);
		cs->depth--;
	}
	cs->len = saved;
	cs->path[cs->len] = '\0';
}

/* Every entry of the directory the path currently names. */
static void
census_dir(struct census *cs)
{
	vnode_t dvp = NULLVP;
	char *dbuf;
	off_t offset = 0;
	int error;

	error = vnode_lookup(cs->path, VNODE_LOOKUP_NOFOLLOW, &dvp, cs->ctx);
	if (error) {
		printf("apfs-check: %s: lookup failed: %d\n", cs->path + cs->rel, error);
		cs->errors++;
		return;
	}
	dbuf = kalloc_data(DIR_CHUNK, Z_WAITOK);
	if (dbuf == NULL) {
		vnode_put(dvp);
		cs->errors++;
		return;
	}
	for (;;) {
		uio_t uio;
		char uio_buf[UIO_SIZEOF(1)];
		int eof = 0, items = 0;
		size_t got;
		char *p;

		uio = uio_createwithbuffer(1, offset, UIO_SYSSPACE, UIO_READ, uio_buf,
		    sizeof(uio_buf));
		if (uio == NULL) {
			cs->errors++;
			break;
		}
		uio_addiov(uio, CAST_USER_ADDR_T(dbuf), DIR_CHUNK);
		error = VNOP_READDIR(dvp, uio, 0, &eof, &items, cs->ctx);
		if (error) {
			printf("apfs-check: %s: readdir failed: %d\n", cs->path + cs->rel, error);
			cs->errors++;
			break;
		}
		got = DIR_CHUNK - (size_t)uio_resid(uio);
		if (got == 0) {
			break;
		}
		offset = uio_offset(uio);

		for (p = dbuf; p < dbuf + got;) {
			struct dirent *de = (struct dirent *)p;

			if (de->d_reclen == 0 || p + de->d_reclen > dbuf + got) {
				printf("apfs-check: %s: a directory entry of %u bytes\n",
				    cs->path + cs->rel, de->d_reclen);
				cs->errors++;
				break;
			}
			p += de->d_reclen;
			if (de->d_namlen == 0 ||
			    (de->d_namlen == 1 && de->d_name[0] == '.') ||
			    (de->d_namlen == 2 && de->d_name[0] == '.' && de->d_name[1] == '.')) {
				continue;
			}
			census_child(cs, de->d_name, de->d_namlen, de->d_type);
		}
		if (eof) {
			break;
		}
	}
	kfree_data(dbuf, DIR_CHUNK);
	vnode_put(dvp);
}

/* ---- getting there ---- */

static int
make_mount_point(vfs_context_t ctx)
{
	struct componentname cn;
	struct vnode_attr va;
	vnode_t dvp = NULLVP, vp = NULLVP;
	char leaf[] = CHECK_LEAF;
	int error;

	error = vnode_lookup(CHECK_PARENT, 0, &dvp, ctx);
	if (error) {
		return error;
	}
	bzero(&cn, sizeof(cn));
	cn.cn_nameiop = CREATE;
	cn.cn_flags = ISLASTCN;
	cn.cn_context = ctx;
	cn.cn_pnbuf = leaf;
	cn.cn_pnlen = (int)sizeof(leaf);
	cn.cn_nameptr = leaf;
	cn.cn_namelen = (int)(sizeof(leaf) - 1);

	VATTR_INIT(&va);
	VATTR_SET(&va, va_type, VDIR);
	VATTR_SET(&va, va_mode, 0755);

	error = VNOP_MKDIR(dvp, &vp, &cn, &va, ctx);
	if (error == 0 && vp != NULLVP) {
		vnode_put(vp);
	}
	vnode_put(dvp);
	if (error == EEXIST) {
		error = 0;
	}
	return error;
}

static void
apfs_check_thread(void *arg, __unused wait_result_t wr)
{
	uint32_t mode = (uint32_t)(uintptr_t)arg;
	vfs_context_t ctx = vfs_context_kernel();
	struct census cs;
	char devname[] = CHECK_DEV;
	char subtree[256] = "";
	uint64_t start, end;
	int error, tries;

	bzero(&cs, sizeof(cs));
	cs.ctx = ctx;
	cs.print_lines = (mode >= 2);

	/*
	 * /private/var is a ramfs that userland's init mounts, so the mount point cannot be
	 * made until that has happened.  Wait for it rather than guess how long it takes.
	 */
	for (tries = 0; tries < 180; tries++) {
		error = make_mount_point(ctx);
		if (error == 0) {
			break;
		}
		delay_for_interval(500, NSEC_PER_MSEC);
	}
	if (error) {
		printf("apfs-check: cannot make %s: %d\n", CHECK_MNT, error);
		return;
	}

	/*
	 * The mount data is the device's path itself, not a pointer to it: the extra
	 * indirection imageboot_pivot_image() uses is unpacked only on the BaseSystem path,
	 * which this build does not have, so apfs_mount() reads the string straight off
	 * this pointer.
	 */
	error = kernel_mount("apfs", NULLVP, NULLVP, CHECK_MNT, devname, 0,
	    MNT_RDONLY | MNT_DONTBROWSE, KERNEL_MOUNT_NOAUTH, ctx);
	if (error) {
		printf("apfs-check: mounting %s on %s failed: %d\n", CHECK_DEV, CHECK_MNT,
		    error);
		return;
	}
	printf("apfs-check: %s mounted on %s\n", CHECK_DEV, CHECK_MNT);
	if (mode >= 3) {
		/* someone in user space is going to read it; leave it alone */
		printf("apfs-check: left mounted, not walking it here\n");
		return;
	}

	cs.path = kalloc_data(PATH_CAP, Z_WAITOK | Z_ZERO);
	cs.buf = kalloc_data(READ_CHUNK, Z_WAITOK);
	if (cs.path == NULL || cs.buf == NULL) {
		printf("apfs-check: out of memory\n");
		return;
	}
	cs.rel = sizeof(CHECK_MNT) - 1;
	strlcpy(cs.path, CHECK_MNT, PATH_CAP);
	cs.len = cs.rel;
	if (PE_parse_boot_arg_str("apfs_subtree", subtree, sizeof(subtree)) &&
	    subtree[0] == '/') {
		strlcat(cs.path, subtree, PATH_CAP);
		cs.len = strlen(cs.path);
		while (cs.len > cs.rel && cs.path[cs.len - 1] == '/') {
			cs.path[--cs.len] = '\0';
		}
		printf("apfs-check: walking %s\n", cs.path + cs.rel);
	} else {
		printf("apfs-check: walking the whole volume\n");
	}

	start = mach_absolute_time();
	census_dir(&cs);
	end = mach_absolute_time();

	{
		uint64_t ns = 0;

		absolutetime_to_nanoseconds(end - start, &ns);
		printf("apfs-check: done in %llu ms\n", ns / 1000000);
	}
	printf("apfs-check: entries %llu  dirs %llu  files %llu  symlinks %llu  other %llu\n",
	    cs.entries, cs.dirs, cs.files, cs.links, cs.others);
	printf("apfs-check: content bytes %llu\n", cs.bytes);
	printf("apfs-check: line-hash sum %016llx\n", (unsigned long long)cs.sum);
	printf("apfs-check: errors %u\n", cs.errors);
	printf("apfs-check: compare with tools/apfs/verify.py sum\n");

	kfree_data(cs.path, PATH_CAP);
	kfree_data(cs.buf, READ_CHUNK);
}

void
redwood_apfs_check_init(void)
{
	uint32_t mode = 0;
	thread_t th;

	if (!PE_parse_boot_argn("apfs_check", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	if (kernel_thread_start(apfs_check_thread, (void *)(uintptr_t)mode, &th) !=
	    KERN_SUCCESS) {
		printf("apfs-check: cannot start the thread\n");
		return;
	}
	thread_deallocate(th);
}
