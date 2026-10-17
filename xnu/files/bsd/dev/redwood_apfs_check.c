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
 *   apfs_check=4              mount nothing: walk the root filesystem, which is what the
 *                             root already is when the volume was handed to the kernel as
 *                             its root device.  This is how the phone is reached: it has
 *                             no driver for its own storage and no virtio, so the only
 *                             block device there is a RAM disk, and a RAM disk given to
 *                             the kernel as the root is mounted by vfs_mountroot.
 *   apfs_check=5              several threads walk the root filesystem at once, each over
 *                             the whole of it, to see what the one lock round apfs_lib
 *                             costs when there is more than one reader
 *   apfs_threads=N            how many, for mode 5 (default 4)
 *   apfs_check=6              mount apfs_dev on apfs_mnt, walk it on this thread and go
 *                             back to the loader.  For a volume that is not the root and
 *                             where there is no user space to make a mount point: the
 *                             point is a directory of the root image itself.
 *   apfs_dev=md1              the device to mount for modes 1 to 3 and 6 (default rvblk0)
 *   apfs_mnt=/lib             where to mount it for mode 6 (default /private/var/apfs-check)
 *   apfs_depth=N              go no deeper than N levels below where the walk starts
 *                             (1 = only the entries of that directory; 0 = no limit).
 *                             For splitting a volume too big to hold into parts: one part
 *                             takes the root's own entries, the others take a subtree
 *                             each, and the per-line hashes add up to the whole.
 *   apfs_skip=/a,/b           directories not to go into, as paths inside the volume,
 *                             comma separated.  Their own line is still counted, only
 *                             their contents are left out -- which is how a volume too
 *                             big to hold is cut into parts that between them cover it
 *                             exactly once.
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
#include <libkern/OSAtomic.h>
#include <sys/redwood_apfs_check.h>

extern void loader_return(void);        /* osfmk/arm64/loader_return.c */
extern void apfs_stats_print(void);     /* bsd/miscfs/apfs/apfs_vfsops.c */
extern void redwood_sparse_report(void);

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
	/*
	 * The volume being walked.  Walking the root means walking over the mount points of
	 * other filesystems -- devfs on /dev above all -- and their contents are not this
	 * volume's, so anything that turns out to live on another mount is passed over.
	 */
	mount_t   mp;
	/*
	 * NOCROSSMOUNT when the volume being walked is the root: a lookup then gives the
	 * directory this volume has under a mount point rather than the root of whatever is
	 * mounted on it, so /dev is counted as the (empty) directory it is here instead of
	 * being either skipped or walked as devfs.  Not set when the volume is reached
	 * through a path that itself crosses into it.
	 */
	int       lookup_flags;
	uint32_t  max_depth;            /* 0: no limit */
	char     *skip;                 /* comma separated paths not to descend into */
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

static bool
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
	error = vnode_lookup(cs->path, cs->lookup_flags, &vp, cs->ctx);
	if (error) {
		printf("apfs-check: %s: lookup failed: %d\n", cs->path + cs->rel, error);
		cs->errors++;
		return false;
	}
	if (cs->mp != NULL && vnode_mount(vp) != cs->mp) {
		vnode_put(vp);          /* another filesystem is mounted here */
		return false;
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
	return error == 0;
}

static void census_dir(struct census *cs);

/* Is this directory one of the ones to stay out of?  Compares the path inside the volume. */
static bool
census_skipped(struct census *cs)
{
	const char *want = cs->path + cs->rel;
	size_t want_len = strlen(want);
	const char *at = cs->skip;

	if (at == NULL) {
		return false;
	}
	while (*at != '\0') {
		const char *end = strchr(at, ',');
		size_t n = (end != NULL) ? (size_t)(end - at) : strlen(at);

		if (n == want_len && memcmp(at, want, n) == 0) {
			return true;
		}
		if (end == NULL) {
			break;
		}
		at = end + 1;
	}
	return false;
}

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

	if (census_entry(cs, d_type) && d_type == DT_DIR && cs->depth < MAX_DEPTH &&
	    (cs->max_depth == 0 || (uint32_t)cs->depth + 1 < cs->max_depth) &&
	    !census_skipped(cs)) {
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

	error = vnode_lookup(cs->len != 0 ? cs->path : "/", cs->lookup_flags, &dvp,
	    cs->ctx);
	if (error == 0 && cs->mp == NULL) {
		cs->mp = vnode_mount(dvp);
	}
	if (error) {
		printf("apfs-check: %s: lookup failed: %d\n",
		    cs->len != 0 ? cs->path + cs->rel : "/", error);
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

#define MAX_THREADS 8

static volatile int apfs_check_running;

struct walk_job {
	int      index;
	uint64_t ms;
	uint64_t entries;
	uint64_t bytes;
	uint64_t sum;
	uint32_t errors;
};

static struct walk_job apfs_jobs[MAX_THREADS];

/* One reader: the whole root filesystem, with everything of its own. */
static void
apfs_walk_one(void *arg, __unused wait_result_t wr)
{
	struct walk_job *job = arg;
	struct census cs;
	uint64_t start, end, ns = 0;

	bzero(&cs, sizeof(cs));
	cs.ctx = vfs_context_kernel();
	PE_parse_boot_argn("apfs_depth", &cs.max_depth, sizeof(cs.max_depth));
	{
		static char skip[256];

		if (PE_parse_boot_arg_str("apfs_skip", skip, sizeof(skip)) && skip[0] != '\0') {
			cs.skip = skip;
			printf("apfs-check: staying out of %s\n", skip);
		}
	}
	cs.lookup_flags = VNODE_LOOKUP_NOFOLLOW | VNODE_LOOKUP_NOCROSSMOUNT;
	cs.path = kalloc_data(PATH_CAP, Z_WAITOK | Z_ZERO);
	cs.buf = kalloc_data(READ_CHUNK, Z_WAITOK);
	if (cs.path == NULL || cs.buf == NULL) {
		printf("apfs-check: reader %d: out of memory\n", job->index);
		OSDecrementAtomic(&apfs_check_running);
		return;
	}
	start = mach_absolute_time();
	census_dir(&cs);
	end = mach_absolute_time();
	absolutetime_to_nanoseconds(end - start, &ns);

	job->ms = ns / 1000000;
	job->entries = cs.entries;
	job->bytes = cs.bytes;
	job->sum = cs.sum;
	job->errors = cs.errors;
	printf("apfs-check: reader %d: %llu entries  %llu bytes  sum %016llx  errors %u  "
	    "%llu ms\n", job->index, cs.entries, cs.bytes,
	    (unsigned long long)cs.sum, cs.errors, job->ms);

	kfree_data(cs.path, PATH_CAP);
	kfree_data(cs.buf, READ_CHUNK);
	OSDecrementAtomic(&apfs_check_running);
}

/*
 * Several readers at once over the same volume.  What is wanted is not only that nothing
 * breaks but what the lock costs: all of apfs_lib runs under one mutex, so the part of the
 * work that is held time cannot go faster however many processors there are.
 */
static void
apfs_check_concurrent(void)
{
	uint32_t want = 4;
	uint64_t start, end, ns = 0;
	uint64_t total_bytes = 0, total_entries = 0;
	int i, n, bad = 0;

	if (PE_parse_boot_argn("apfs_threads", &want, sizeof(want)) == 0 || want == 0) {
		want = 4;
	}
	if (want > MAX_THREADS) {
		want = MAX_THREADS;
	}
	n = (int)want;
	printf("apfs-check: %d readers over the root filesystem at once\n", n);

	bzero(apfs_jobs, sizeof(apfs_jobs));
	apfs_check_running = n;
	start = mach_absolute_time();
	for (i = 0; i < n; i++) {
		thread_t th;

		apfs_jobs[i].index = i;
		if (kernel_thread_start(apfs_walk_one, &apfs_jobs[i], &th) != KERN_SUCCESS) {
			printf("apfs-check: cannot start reader %d\n", i);
			OSDecrementAtomic(&apfs_check_running);
			continue;
		}
		thread_deallocate(th);
	}
	while (apfs_check_running > 0) {
		delay_for_interval(50, NSEC_PER_MSEC);
	}
	end = mach_absolute_time();
	absolutetime_to_nanoseconds(end - start, &ns);

	for (i = 0; i < n; i++) {
		total_bytes += apfs_jobs[i].bytes;
		total_entries += apfs_jobs[i].entries;
		if (apfs_jobs[i].sum != apfs_jobs[0].sum ||
		    apfs_jobs[i].entries != apfs_jobs[0].entries || apfs_jobs[i].errors != 0) {
			bad++;
		}
	}
	printf("apfs-check: %d readers done in %llu ms\n", n, ns / 1000000);
	printf("apfs-check: together %llu entries  %llu bytes  %llu KB/s\n", total_entries,
	    total_bytes, ns ? (total_bytes * 1000000ull) / ns : 0);
	printf("apfs-check: readers that disagreed or failed: %d\n", bad);
	apfs_stats_print();
}

static void
apfs_check_thread(void *arg, __unused wait_result_t wr)
{
	uint32_t mode = (uint32_t)(uintptr_t)arg;
	vfs_context_t ctx = vfs_context_kernel();
	struct census cs;
	char devname[64] = CHECK_DEV;
	char subtree[256] = "";
	uint64_t start, end;
	int error, tries;

	bzero(&cs, sizeof(cs));
	cs.ctx = ctx;
	cs.print_lines = (mode == 2);
	/*
	 * NOCROSSMOUNT only where the volume is the root (modes 4 and 5).  In mode 6 the
	 * path leads from the root image into the mount, so crossing has to be allowed; the
	 * walk is kept on the volume by the check against the mount it started on.
	 */
	PE_parse_boot_argn("apfs_depth", &cs.max_depth, sizeof(cs.max_depth));
	{
		static char skip[256];

		if (PE_parse_boot_arg_str("apfs_skip", skip, sizeof(skip)) && skip[0] != '\0') {
			cs.skip = skip;
			printf("apfs-check: staying out of %s\n", skip);
		}
	}
	cs.lookup_flags = VNODE_LOOKUP_NOFOLLOW |
	    ((mode == 4 || mode == 5) ? VNODE_LOOKUP_NOCROSSMOUNT : 0);
	if (PE_parse_boot_arg_str("apfs_dev", subtree, sizeof(subtree)) && subtree[0] != '\0') {
		snprintf(devname, sizeof(devname), "/dev/%s", subtree);
	}
	subtree[0] = '\0';

	cs.path = kalloc_data(PATH_CAP, Z_WAITOK | Z_ZERO);
	cs.buf = kalloc_data(READ_CHUNK, Z_WAITOK);
	if (cs.path == NULL || cs.buf == NULL) {
		printf("apfs-check: out of memory\n");
		return;
	}

	if (mode == 6) {
		char mnt[128] = CHECK_MNT;

		if (PE_parse_boot_arg_str("apfs_mnt", subtree, sizeof(subtree)) &&
		    subtree[0] == '/') {
			strlcpy(mnt, subtree, sizeof(mnt));
		}
		subtree[0] = '\0';
		/*
		 * A node just made with devfs_make_node() is not in /dev the instant it is
		 * made -- this runs straight after the device registered itself, and the first
		 * lookup comes back ENOENT -- so wait for it rather than guess how long devfs
		 * takes to show it.
		 */
		{
			vnode_t t = NULLVP;
			int tries;

			for (tries = 0; tries < 100; tries++) {
				if (vnode_lookup(devname, 0, &t, ctx) == 0) {
					vnode_put(t);
					break;
				}
				delay_for_interval(20, NSEC_PER_MSEC);
			}
			if (tries >= 100) {
				printf("apfs-check: %s never showed up in /dev\n", devname);
			} else if (tries > 0) {
				printf("apfs-check: %s showed up in /dev after %d ms\n", devname,
				    tries * 20);
			}
		}
		error = kernel_mount("apfs", NULLVP, NULLVP, mnt, devname, 0,
		    MNT_RDONLY | MNT_DONTBROWSE, KERNEL_MOUNT_NOAUTH, ctx);
		if (error) {
			printf("apfs-check: mounting %s on %s failed: %d\n", devname, mnt,
			    error);
			kfree_data(cs.path, PATH_CAP);
			kfree_data(cs.buf, READ_CHUNK);
			return;
		}
		printf("apfs-check: %s mounted on %s\n", devname, mnt);
		cs.rel = strlen(mnt);
		strlcpy(cs.path, mnt, PATH_CAP);
		cs.len = cs.rel;
		if (PE_parse_boot_arg_str("apfs_subtree", subtree, sizeof(subtree)) &&
		    subtree[0] == '/') {
			strlcat(cs.path, subtree, PATH_CAP);
			cs.len = strlen(cs.path);
			printf("apfs-check: walking %s\n", cs.path + cs.rel);
		} else {
			printf("apfs-check: walking the whole volume\n");
		}
		goto walk;
	}
	if (mode >= 5) {
		kfree_data(cs.path, PATH_CAP);
		kfree_data(cs.buf, READ_CHUNK);
		apfs_check_concurrent();
		return;
	}
	if (mode >= 4 && mode != 6) {
		/*
		 * The volume is already the root: nothing to mount, and no mount point to
		 * make, so none of what follows about /private/var applies.
		 */
		cs.rel = 0;
		cs.len = 0;
		cs.path[0] = '\0';
		printf("apfs-check: walking the root filesystem\n");
		goto walk;
	}

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
		printf("apfs-check: mounting %s on %s failed: %d\n", devname, CHECK_MNT,
		    error);
		return;
	}
	printf("apfs-check: %s mounted on %s\n", devname, CHECK_MNT);
	if (mode >= 3) {
		/* someone in user space is going to read it; leave it alone */
		printf("apfs-check: left mounted, not walking it here\n");
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

walk:
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
	apfs_stats_print();
	redwood_sparse_report();
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
	if (mode >= 4) {
		/*
		 * The volume is the root (or mode 6, which mounts it here), so everything the
		 * walk needs is already up -- and
		 * bsd_init is about to exec /sbin/launchd off it, which on anything but our
		 * own packfs image ends in a panic before a thread of our own would finish.
		 * So walk it here, on this thread, and go back to the loader with the answer
		 * rather than letting that happen.
		 */
		apfs_check_thread((void *)(uintptr_t)mode, 0);
		printf("apfs-check: going back to the loader\n");
		loader_return();
		return;
	}
	if (kernel_thread_start(apfs_check_thread, (void *)(uintptr_t)mode, &th) !=
	    KERN_SUCCESS) {
		printf("apfs-check: cannot start the thread\n");
		return;
	}
	thread_deallocate(th);
}
