/*
 * Process 1 of the Darwin bring-up, until it hands over to the real launchd.
 *
 * It lives in the small image mounted on /redwood (see stage0.c) and
 *   - mounts a memory filesystem on /private/var and creates the directories
 *     a system expects there;
 *   - checks what the kernel can do for user space, one line per check;
 *   - runs the commands listed in etc/rc of its image: usually just
 *     "exec /sbin/launchd.ios", which turns process 1 into the iOS launchd;
 *   - halts the machine if it is still process 1 after that.
 *
 * Started with a file name by anything else, it is a command runner: that is
 * how a launchd job (LaunchDaemons/com.redwood.demo.plist) runs a list of
 * commands.
 */
#include "mini.h"

#define PATTERN_SIZE    300000
#define RB_HALT         0x08

static int checks, failures;

/* Where this program's own image is mounted ("" when it is the root itself). */
static const char *base = "";

/* A path inside our own image; the result is valid until the next call. */
static const char *
in_image(const char *path)
{
	static char buf[256];

	strcpy(buf, base);
	strcpy(buf + strlen(buf), path);
	return buf;
}

static void
check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		failures++;
	}
	printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
}

/* Same formula as build.sh uses to generate the file. */
static unsigned char
pattern(unsigned long i)
{
	return (unsigned char)(i * 31 + (i >> 8));
}

static void
show_system(void)
{
	char type[32] = "?", release[32] = "?", machine[32] = "?", version[256] = "?";
	unsigned long long memsize = 0, pagesize = 0;
	int ncpu = 0;
	size_t len;

	len = sizeof(type), sysctlbyname("kern.ostype", type, &len, NULL, 0);
	len = sizeof(release), sysctlbyname("kern.osrelease", release, &len, NULL, 0);
	len = sizeof(version), sysctlbyname("kern.version", version, &len, NULL, 0);
	len = sizeof(machine), sysctlbyname("hw.machine", machine, &len, NULL, 0);
	len = sizeof(memsize), sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
	len = sizeof(pagesize), sysctlbyname("hw.pagesize", &pagesize, &len, NULL, 0);
	len = sizeof(ncpu), sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0);

	printf("%s %s %s\n%s\n", type, release, machine, version);
	printf("memory %llu MB, page size %llu, %d cpu\n\n", memsize >> 20, pagesize, ncpu);
	check(strcmp(type, "Darwin") == 0 && memsize != 0, "sysctl: kernel identifies itself");
}

static void
mode_string(unsigned mode, char *out)
{
	static const char bits[] = "rwxrwxrwx";

	out[0] = (mode & S_IFMT) == S_IFDIR ? 'd' : (mode & S_IFMT) == S_IFLNK ? 'l' :
	    (mode & S_IFMT) == S_IFCHR ? 'c' : (mode & S_IFMT) == S_IFBLK ? 'b' : (mode & S_IFMT) == S_IFSOCK ? 's' : '-';
	for (int i = 0; i < 9; i++) {
		out[1 + i] = (mode & (0400 >> i)) ? bits[i] : '-';
	}
	out[10] = 0;
}

/* ls -lR; returns the number of entries seen, or -1 */
static int
list_tree(const char *path, int depth)
{
	static char buf[8192];          /* one directory at a time: the contents are copied out before recursing */
	char names[32][64];
	int count = 0, subdirs = 0, total = 0;
	off_t position = 0;
	ssize_t n;
	int fd = open(path, O_RDONLY | O_DIRECTORY, 0);

	if (fd < 0) {
		printf("  %s: %s\n", path, strerror(errno));
		return -1;
	}
	while ((n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
		for (ssize_t at = 0; at < n;) {
			struct dirent *de = (struct dirent *)(buf + at);
			char full[256], mode[12], target[128];
			struct stat st;

			at += de->d_reclen;
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
				continue;
			}
			strcpy(full, path);
			if (full[strlen(full) - 1] != '/') {
				strcpy(full + strlen(full), "/");
			}
			strcpy(full + strlen(full), de->d_name);
			if (lstat(full, &st) != 0) {
				printf("  %s: %s\n", full, strerror(errno));
				close(fd);
				return -1;
			}
			mode_string(st.st_mode, mode);
			printf("  %s %2u %8lld %s", mode, st.st_nlink, st.st_size, full);
			if ((st.st_mode & S_IFMT) == S_IFLNK) {
				ssize_t t = readlink(full, target, sizeof(target) - 1);

				target[t > 0 ? t : 0] = 0;
				printf(" -> %s", target);
			}
			printf("\n");
			count++;
			if (de->d_type == DT_DIR && depth > 0 && subdirs < 32 && strlen(full) < 64) {
				strcpy(names[subdirs++], full);
			}
		}
	}
	close(fd);
	if (n < 0) {
		return -1;
	}
	total = count;
	for (int i = 0; i < subdirs; i++) {
		int sub = list_tree(names[i], depth - 1);

		if (sub < 0) {
			return -1;
		}
		total += sub;
	}
	return total;
}

static void
test_files(void)
{
	static unsigned char buf[65536];
	char text[128];
	struct stat st;
	int fd, good;
	ssize_t n;
	unsigned long done;
	unsigned char *map;

	printf("\n-- read-only filesystem --\n");
	check(list_tree(*base ? base : "/", 3) >= 8, "packfs: walk our image (readdir, lstat, readlink)");
	check(stat("/usr/lib/dyld", &st) != 0 || st.st_size > 100000, "the system image has the dynamic linker");

	fd = open(in_image("/etc/motd"), O_RDONLY, 0);
	n = fd >= 0 ? read(fd, text, sizeof(text) - 1) : -1;
	text[n > 0 ? n : 0] = 0;
	close(fd);
	printf("  etc/motd: %s", text);
	check(n > 0 && strncmp(text, "Darwin", 6) == 0, "read a small file");

	check(stat("/tmp", &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR, "follow a symlink to a directory");
	check(open("/nonexistent", O_RDONLY, 0) < 0 && errno == 2, "missing file gives ENOENT");
	check(open(in_image("/etc/newfile"), O_WRONLY | O_CREAT, 0644) < 0 && errno == 30, "creating a file gives EROFS");

	fd = open(in_image("/share/pattern.bin"), O_RDONLY, 0);
	good = fd >= 0 && fstat(fd, &st) == 0 && st.st_size == PATTERN_SIZE;
	for (done = 0; good && done < PATTERN_SIZE; done += (unsigned long)n) {
		n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			good = 0;
			break;
		}
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] != pattern(done + (unsigned long)i)) {
				good = 0;
			}
		}
	}
	check(good && done == PATTERN_SIZE, "read a 300 KB file and verify every byte");

	good = fd >= 0 && lseek(fd, 123457, SEEK_SET) == 123457 && read(fd, buf, 1000) == 1000;
	for (int i = 0; good && i < 1000; i++) {
		good = buf[i] == pattern(123457 + (unsigned long)i);
	}
	check(good, "seek into the middle and read");

	map = fd >= 0 ? mmap(NULL, PATTERN_SIZE, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
	good = map != MAP_FAILED;
	for (unsigned long i = 0; good && i < PATTERN_SIZE; i++) {
		good = map[i] == pattern(i);
	}
	check(good, "mmap the file and verify every byte (page-in)");
	if (map != MAP_FAILED) {
		munmap(map, PATTERN_SIZE);
	}
	close(fd);
}

static void
test_memory(void)
{
	const size_t size = 64u << 20, step = 4096;
	unsigned long *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	int good = mem != MAP_FAILED;

	printf("\n-- memory --\n");
	for (size_t off = 0; good && off < size; off += step) {
		mem[off / sizeof(*mem)] = off ^ 0x5a5a5a5a5a5a5a5aUL;
	}
	for (size_t off = 0; good && off < size; off += step) {
		good = mem[off / sizeof(*mem)] == (off ^ 0x5a5a5a5a5a5a5a5aUL);
	}
	check(good, "64 MB of anonymous memory: touch every page, read it back");
	check(good && munmap(mem, size) == 0, "unmap it");
}

static void
test_processes(void)
{
	char program[128];
	char *argv[] = { program, "one", "two", NULL };
	char *envp[] = { "GREETING=hello from init", NULL };
	int status = 0, fds[2];
	char text[16] = "";
	pid_t pid;

	strcpy(program, in_image("/bin/child"));
	printf("\n-- processes --\n");
	pid = fork();
	if (pid == 0) {
		execve(argv[0], argv, envp);
		printf("  execve: %s\n", strerror(errno));
		_exit(127);
	}
	check(pid > 1, "fork");
	check(pid > 0 && wait4(pid, &status, 0, NULL) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42,
	    "exec a second program from the image, collect its exit status");

	if (pipe(fds) == 0) {
		pid = fork();
		if (pid == 0) {
			close(fds[0]);
			write(fds[1], "ping", 4);
			_exit(0);
		}
		close(fds[1]);
		read(fds[0], text, sizeof(text) - 1);
		close(fds[0]);
		wait4(pid, &status, 0, NULL);
	}
	check(strcmp(text, "ping") == 0, "pipe from a child process");
}

/*
 * Present only in the image that carries the iOS dyld and shared cache:
 * an ordinary dynamically linked program.
 */
static void
test_dynamic(void)
{
	char program[128];
	char *argv[] = { program, "from", "init", NULL };
	char *envp[] = { "PATH=/bin:/usr/bin", "HOME=/", NULL };
	struct stat st;
	int status = 0;
	pid_t pid;

	strcpy(program, in_image("/bin/hello_dyn"));
	if (stat(argv[0], &st) != 0 || stat("/usr/lib/dyld", &st) != 0) {
		return;
	}
	printf("\n-- dynamic linking --\n");
	pid = fork();
	if (pid == 0) {
		execve(argv[0], argv, envp);
		printf("  execve: %s\n", strerror(errno));
		_exit(127);
	}
	if (pid > 0 && wait4(pid, &status, 0, NULL) == pid) {
		if (WIFEXITED(status)) {
			printf("  %s exited with status %d\n", argv[0], WEXITSTATUS(status));
		} else {
			printf("  %s was killed by signal %d\n", argv[0], WTERMSIG(status));
		}
	}
	check(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 7, "run a program through dyld and the shared cache");
}

/* One line of the skeleton list: fields are separated by tabs. Returns the number of fields found. */
static int
split_tabs(char *line, char **fields, int max)
{
	int n = 0;

	while (n < max) {
		fields[n++] = line;
		line = strchr(line, '\t');
		if (line == NULL) {
			break;
		}
		*line++ = 0;
	}
	return n;
}

static long
parse_number(const char *text, int radix)
{
	long value = 0;

	for (; *text >= '0' && *text < '0' + radix; text++) {
		value = value * radix + (*text - '0');
	}
	return value;
}

/*
 * /private/var is an empty directory of the read-only root: give it a
 * memory-backed filesystem and the directories a system expects there.
 * (/tmp is a symlink to /private/var/tmp, as on iOS.)
 */
static int
copy_file(const char *from, const char *to, int mode, int uid, int gid)
{
	static char buf[4096];
	int in = open(from, O_RDONLY, 0), out, ok = 0;
	ssize_t n;

	if (in < 0) {
		return 0;
	}
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out >= 0) {
		ok = 1;
		while ((n = read(in, buf, sizeof(buf))) > 0) {
			ok = ok && write(out, buf, (size_t)n) == n;
		}
		close(out);
		ok = ok && n == 0 && chmod(to, mode) == 0 && chown(to, uid, gid) == 0;
	}
	close(in);
	return ok;
}

static void
mount_writable(void)
{
	static char skeleton[16384];
	static const char *const markers[] = { "/private/var/db/diagnostics/.migrated",
		                               "/private/var/db/diagnostics/timesync/.migrated", "/private/var/db/uuidtext/.migrated" };
	int ok, fd, made = 0, files = 0;
	ssize_t n;

	printf("\n-- writable filesystem --\n");
	ok = mount("ramfs", "/private/var", 0, NULL) == 0;
	if (!ok) {
		printf("  mount: %s\n", strerror(errno));
	}
	check(ok, "mount a memory filesystem on /private/var");

	/* the directories of the data volume, as listed in the template that ships with iOS (ios165/mkvarskel.py) */
	fd = open(in_image("/etc/var.skel"), O_RDONLY, 0);
	n = fd >= 0 ? read(fd, skeleton, sizeof(skeleton) - 1) : -1;
	close(fd);
	skeleton[n > 0 ? n : 0] = 0;
	for (char *line = skeleton; ok && *line;) {
		char *end = strchr(line, '\n'), *f[6];
		int count;

		if (end) {
			*end = 0;
		}
		count = split_tabs(line, f, 6);
		if (count == 5 && f[0][0] == 'd') {
			int mode = (int)parse_number(f[1], 8);

			ok = mkdir(f[4], mode) == 0 && chmod(f[4], mode) == 0 &&
			    chown(f[4], (int)parse_number(f[2], 10), (int)parse_number(f[3], 10)) == 0;
			if (!ok) {
				printf("  %s: %s\n", f[4], strerror(errno));
			}
			made++;
		} else if (count == 3 && f[0][0] == 'l') {
			symlink(f[1], f[2]);
		} else if (count == 6 && f[0][0] == 'f') {
			/* a file of ours to start with (build.sh: the var/ directory): mode, owner, group, path, source in the image */
			if (!copy_file(in_image(f[5]), f[4], (int)parse_number(f[1], 8), (int)parse_number(f[2], 10),
			    (int)parse_number(f[3], 10))) {
				printf("  %s: %s\n", f[4], strerror(errno));
			}
			files++;
		}
		if (!end) {
			break;
		}
		line = end + 1;
	}
	/*
	 * logd takes a log directory without this marker for one left by an
	 * older system, to be converted by logd_helper; these are brand new.
	 */
	for (unsigned i = 0; ok && i < sizeof(markers) / sizeof(markers[0]); i++) {
		fd = open(markers[i], O_WRONLY | O_CREAT, 0644);
		ok = fd >= 0 && chown(markers[i], 272, 272) == 0;
		close(fd);
	}
	printf("  %d directories, %d files\n", made, files);
	check(ok && made > 50, "create the directories of the data volume");
}

static int
count_entries(const char *path)
{
	static char buf[4096];
	off_t position = 0;
	ssize_t n;
	int count = 0, fd = open(path, O_RDONLY | O_DIRECTORY, 0);

	if (fd < 0) {
		return -1;
	}
	while ((n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
		for (ssize_t at = 0; at < n; at += ((struct dirent *)(buf + at))->d_reclen) {
			const char *name = ((struct dirent *)(buf + at))->d_name;

			count += strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
		}
	}
	close(fd);
	return n < 0 ? -1 : count;
}

static void
test_writable(void)
{
	static unsigned char buf[65536];
	const unsigned long total = 100000;
	struct sockaddr_un sun = { .sun_len = sizeof(sun), .sun_family = AF_UNIX, .sun_path = "/tmp/t/socket" };
	struct stat st;
	char text[64];
	unsigned char *map;
	unsigned long done;
	int fd, fd2, good, s;
	ssize_t n;

	if (mkdir("/tmp/t", 0755) != 0) {
		return;         /* no writable filesystem in this configuration */
	}
	printf("\n-- writing files --\n");

	fd = open("/tmp/t/file", O_RDWR | O_CREAT | O_EXCL, 0644);
	good = fd >= 0;
	for (done = 0; good && done < total; done += (unsigned long)n) {
		unsigned long piece = total - done < 7777 ? total - done : 7777;

		for (unsigned long i = 0; i < piece; i++) {
			buf[i] = pattern(done + i);
		}
		n = write(fd, buf, piece);
		good = n == (ssize_t)piece;
	}
	good = good && fstat(fd, &st) == 0 && st.st_size == (off_t)total && lseek(fd, 0, SEEK_SET) == 0;
	for (done = 0; good && done < total; done += (unsigned long)n) {
		n = read(fd, buf, sizeof(buf));
		good = n > 0;
		for (ssize_t i = 0; good && i < n; i++) {
			good = buf[i] == pattern(done + (unsigned long)i);
		}
	}
	check(good && done == total, "create a file, write 100 KB, read it back");

	good = pwrite(fd, "0123456789", 10, 50000) == 10 && pread(fd, text, 12, 49999) == 12 &&
	    (unsigned char)text[0] == pattern(49999) && memcmp(text + 1, "0123456789", 10) == 0 &&
	    (unsigned char)text[11] == pattern(50010);
	check(good, "overwrite in the middle");

	fd2 = open("/tmp/t/file", O_WRONLY | O_APPEND, 0);
	good = fd2 >= 0 && write(fd2, "tail!", 5) == 5 && fstat(fd, &st) == 0 && st.st_size == (off_t)total + 5 &&
	    pread(fd, text, 5, (off_t)total) == 5 && memcmp(text, "tail!", 5) == 0;
	close(fd2);
	check(good, "append");

	good = ftruncate(fd, 1000) == 0 && fstat(fd, &st) == 0 && st.st_size == 1000 && ftruncate(fd, 40000) == 0 &&
	    pread(fd, buf, 2000, 0) == 2000;
	for (int i = 0; good && i < 2000; i++) {
		good = buf[i] == (i < 1000 ? pattern((unsigned long)i) : 0);
	}
	good = good && pread(fd, buf, 100, 30000) == 100 && buf[0] == 0 && buf[99] == 0 && pread(fd, buf, 10, 40000) == 0;
	check(good, "truncate, then extend: the new part reads as zeros");

	good = rename("/tmp/t/file", "/tmp/t/renamed") == 0 && stat("/tmp/t/file", &st) != 0 && errno == 2 &&
	    stat("/tmp/t/renamed", &st) == 0 && st.st_size == 40000;
	check(good, "rename");

	good = link("/tmp/t/renamed", "/tmp/t/hard") == 0 && fstat(fd, &st) == 0 && st.st_nlink == 2 &&
	    unlink("/tmp/t/renamed") == 0 && fstat(fd, &st) == 0 && st.st_nlink == 1;
	check(good, "hard link, remove the first name");

	good = unlink("/tmp/t/hard") == 0 && stat("/tmp/t/hard", &st) != 0 && fstat(fd, &st) == 0 && st.st_nlink == 0 &&
	    pread(fd, buf, 100, 0) == 100 && buf[5] == pattern(5) && pwrite(fd, "x", 1, 0) == 1;
	close(fd);
	check(good, "a removed file stays usable while it is open");

	n = symlink("some/where else", "/tmp/t/symlink") == 0 ? readlink("/tmp/t/symlink", text, sizeof(text) - 1) : -1;
	text[n > 0 ? n : 0] = 0;
	check(n == 15 && strcmp(text, "some/where else") == 0 && lstat("/tmp/t/symlink", &st) == 0 &&
	    (st.st_mode & S_IFMT) == S_IFLNK, "symbolic link");

	good = mkdir("/tmp/t/a", 0700) == 0 && mkdir("/tmp/t/a/b", 0700) == 0 && rmdir("/tmp/t/a") != 0 && errno == 66 &&
	    rename("/tmp/t/a", "/tmp/t/c") == 0 && stat("/tmp/t/c/b", &st) == 0 && stat("/tmp/t/c/b/..", &st) == 0 &&
	    rename("/tmp/t/c", "/tmp/t/c/b/inside") != 0 && rmdir("/tmp/t/c/b") == 0 && rmdir("/tmp/t/c") == 0 &&
	    stat("/tmp/t/c", &st) != 0;
	check(good, "directories: nest, refuse to remove a full one, rename, remove");

	fd = open("/tmp/t/mapped", O_RDWR | O_CREAT, 0600);
	map = fd >= 0 && ftruncate(fd, 65536) == 0 ? mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
	good = map != MAP_FAILED;
	if (good) {
		memset(map + 1000, 'M', 100);
		good = pread(fd, buf, 102, 999) == 102 && buf[0] == 0 && buf[1] == 'M' && buf[100] == 'M' && buf[101] == 0;
	}
	check(good, "shared mapping: what is stored through memory is seen by read()");
	good = good && pwrite(fd, "WWWW", 4, 40000) == 4 && map[40000] == 'W' && map[40003] == 'W' && map[1000] == 'M';
	check(good, "and what write() stores is seen through the mapping");
	if (map != MAP_FAILED) {
		munmap(map, 65536);
	}
	close(fd);

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	good = s >= 0 && bind(s, &sun, sizeof(sun)) == 0 && stat(sun.sun_path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFSOCK;
	close(s);
	check(good, "bind a local socket to a name");

	good = mkdir("/tmp/t/many", 0755) == 0;
	for (int i = 0; good && i < 300; i++) {
		char name[32] = "/tmp/t/many/f000";

		name[13] = (char)('0' + i / 100), name[14] = (char)('0' + i / 10 % 10), name[15] = (char)('0' + i % 10);
		fd = open(name, O_WRONLY | O_CREAT, 0644);
		good = fd >= 0 && write(fd, name, 16) == 16;
		close(fd);
	}
	good = good && count_entries("/tmp/t/many") == 300;
	for (int i = 0; good && i < 300; i++) {
		char name[32] = "/tmp/t/many/f000";

		name[13] = (char)('0' + i / 100), name[14] = (char)('0' + i / 10 % 10), name[15] = (char)('0' + i % 10);
		good = unlink(name) == 0;
	}
	check(good && count_entries("/tmp/t/many") == 0 && rmdir("/tmp/t/many") == 0, "300 files in one directory: create, list, remove");

	list_tree("/tmp/t", 1);
}

/* Run one line of /etc/rc: words separated by blanks, no quoting; a last word "&" = do not wait. */
static void
run_command(char *line)
{
	char *argv[64];
	char *envp[] = { "PATH=/bin:/sbin:/usr/bin:/usr/sbin", "HOME=/", "TERM=dumb", NULL };
	int argc = 0, status = 0, background = 0;
	pid_t pid;

	for (char *p = line; *p && argc < 63;) {
		while (*p == ' ' || *p == '\t') {
			*p++ = 0;
		}
		if (!*p) {
			break;
		}
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t') {
			p++;
		}
	}
	argv[argc] = NULL;
	if (argc == 0) {
		return;
	}

	printf("\n$");
	for (int i = 0; i < argc; i++) {
		printf(" %s", argv[i]);
	}
	printf("\n");
	if (argc > 1 && strcmp(argv[argc - 1], "&") == 0) {
		background = 1;
		argv[--argc] = NULL;
	}

	/* "exec program args": become that program, keeping process number 1 */
	if (strcmp(argv[0], "exec") == 0 && argc > 1) {
		execve(argv[1], argv + 1, envp);
		printf("%s: %s\n", argv[1], strerror(errno));
		return;
	}

	pid = fork();
	if (pid == 0) {
		execve(argv[0], argv, envp);
		printf("%s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	if (background && pid > 0) {
		return;
	}
	if (pid < 0 || wait4(pid, &status, 0, NULL) != pid) {
		printf("[could not run: %s]\n", strerror(errno));
	} else if (!WIFEXITED(status)) {
		printf("[killed by signal %d]\n", WTERMSIG(status));
	} else if (WEXITSTATUS(status) != 0) {
		printf("[exit status %d]\n", WEXITSTATUS(status));
	}
}

static void
run_rc(const char *path)
{
	static char script[8192];
	int fd = open(path, O_RDONLY, 0);
	ssize_t n;

	if (fd < 0) {
		return;
	}
	n = read(fd, script, sizeof(script) - 1);
	close(fd);
	script[n > 0 ? n : 0] = 0;

	printf("\n=== %s ===\n", path);
	for (char *line = script; *line;) {
		char *end = strchr(line, '\n');

		if (end) {
			*end = 0;
		}
		if (*line != '#') {
			run_command(line);
		}
		if (!end) {
			break;
		}
		line = end + 1;
	}
}

static void
test_devices(void)
{
	unsigned char buf[16];
	unsigned sum = 0;
	int fd, good;

	printf("\n-- devices --\n");
	check(list_tree("/dev", 0) > 3, "devfs is mounted on /dev");

	fd = open("/dev/zero", O_RDONLY, 0);
	memset(buf, 0xff, sizeof(buf));
	good = fd >= 0 && read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
	for (unsigned i = 0; i < sizeof(buf); i++) {
		sum += buf[i];
	}
	close(fd);
	check(good && sum == 0, "/dev/zero");

	fd = open("/dev/null", O_WRONLY, 0);
	check(fd >= 0 && write(fd, "x", 1) == 1, "/dev/null");
	close(fd);

	fd = open("/dev/random", O_RDONLY, 0);
	memset(buf, 0, sizeof(buf));
	good = fd >= 0 && read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
	close(fd);
	printf("  /dev/random:");
	for (unsigned i = 0; i < sizeof(buf); i++) {
		printf(" %02x", buf[i]);
		sum += buf[i];
	}
	printf("\n");
	check(good && sum != 0, "/dev/random");
}

static void
test_time(void)
{
	struct timeval before = { 0 }, after = { 0 };
	long elapsed;

	printf("\n-- time --\n");
	gettimeofday(&before, NULL);
	usleep(200000);
	gettimeofday(&after, NULL);
	elapsed = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_usec - before.tv_usec) / 1000;
	printf("  slept 200 ms, the clock advanced %ld ms\n", elapsed);
	check(elapsed >= 190 && elapsed < 1000, "sleep and wall clock agree");
}

/*
 * etc/inject.conf is one line, "LIBRARY NAME NAME ...": the kernel starts the programs with
 * these file names with the library inserted (sysctl kern.exec_inject, see libredwood).
 */
static void
configure_inject(void)
{
	char conf[512];
	int fd = open(in_image("/etc/inject.conf"), O_RDONLY, 0);
	long n;

	if (fd < 0) {
		return;
	}
	n = read(fd, conf, sizeof(conf) - 1);
	close(fd);
	while (n > 0 && (conf[n - 1] == '\n' || conf[n - 1] == ' ')) {
		n--;
	}
	if (n <= 0) {
		return;
	}
	conf[n] = 0;
	/* one "LIBRARY NAME..." per line in the file; the kernel wants them separated by ";" */
	for (long i = 0; i < n; i++) {
		if (conf[i] == '\n') {
			conf[i] = ';';
		}
	}
	if (sysctlbyname("kern.exec_inject", NULL, NULL, conf, (size_t)n + 1) == 0) {
		printf("init: library insertion: %s\n", conf);
	} else {
		printf("init: kern.exec_inject: error %d\n", errno);
	}
}

int
main(int argc, char **argv, char **envp)
{
	int fd = open("/dev/console", O_RDWR, 0);

	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2) {
		close(fd);
	}

	(void)envp;
	if (argc > 0 && strncmp(argv[0], "/redwood/", 9) == 0) {
		base = "/redwood";
	}
	/* Not process 1: a command runner, "init FILE" (this is how a launchd job runs a list of commands). */
	if (getpid() != 1) {
		if (argc > 1) {
			run_rc(argv[1]);
		}
		return 0;
	}

	printf("\n=== init: process %d, started as %s ===\n", getpid(), argc > 0 ? argv[0] : "?");

	show_system();
	mount_writable();
	test_files();
	test_writable();
	test_memory();
	test_processes();
	test_dynamic();
	test_devices();
	test_time();

	printf("\n=== %d checks, %d failed ===\n", checks, failures);
	/* daemons that do not run as root get the console as their standard error too */
	chmod("/dev/console", 0666);
	configure_inject();
	run_rc(in_image("/etc/rc"));
	printf("\n=== init: done ===\n");
	reboot(RB_HALT);
	return failures != 0;
}
