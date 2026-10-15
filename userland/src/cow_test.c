/*
 * cow_test: is memory that is copied really a copy? For each of the ways a picture's bytes can go
 * from one place to another -- sent out of line with a virtual copy, vm_copy() into memory that is
 * there already -- a buffer of an icon's size (180 x 180 x 4) is filled, copied, and the source
 * written over afterwards; the copy must still be what the source was, in every 16K page of it.
 * Page-aligned and 4224 bytes into a page (where the broken Calendar icon's bytes lie: its first
 * and last pages shared with what is around it), from memory of our own, from malloc, from
 * purgeable memory, with the source or the destination wired (mlock), and with the received copy
 * made read-only and wired, or with a surface made around it the way CoreAnimation's window server
 * makes one around a picture (src/... IOSurfaceAddress; kernel: MiniIOSurface.cpp).
 * Run by RC_JOBS=rc.jobs.cow; every line should end with 0 pages wrong.
 */
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct sent {
	mach_msg_header_t header;
	mach_msg_body_t body;
	mach_msg_ool_descriptor_t data;
};

struct got {
	struct sent message;
	mach_msg_trailer_t trailer;
};

static void
one(const char *what, unsigned char *buffer, size_t size, int touch_neighbours)
{
	mach_port_t port;
	struct sent m;
	struct got g;
	kern_return_t kr;
	size_t page = (size_t)getpagesize(), changed = 0, pages = 0;
	char line[512] = "";
	size_t used = 0;

	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
	mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
	memset(buffer, 0x11, size);
	memset(&m, 0, sizeof(m));
	m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
	m.header.msgh_size = sizeof(m);
	m.header.msgh_remote_port = port;
	m.body.msgh_descriptor_count = 1;
	m.data.address = buffer;
	m.data.size = (mach_msg_size_t)size;
	m.data.deallocate = 0;
	m.data.copy = MACH_MSG_VIRTUAL_COPY;
	m.data.type = MACH_MSG_OOL_DESCRIPTOR;
	kr = mach_msg(&m.header, MACH_SEND_MSG, sizeof(m), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
	if (kr != KERN_SUCCESS) {
		printf("cow_test: %s: send failed %#x\n", what, kr);
		return;
	}
	if (touch_neighbours) {
		/* what is before and after it in its first and last page, written first */
		uintptr_t first = (uintptr_t)buffer & ~(page - 1), end = ((uintptr_t)buffer + size + page - 1) & ~(page - 1);

		memset((void *)first, 0x33, (uintptr_t)buffer - first);
		memset(buffer + size, 0x33, end - ((uintptr_t)buffer + size));
	}
	memset(buffer, 0x22, size);
	memset(&g, 0, sizeof(g));
	kr = mach_msg(&g.message.header, MACH_RCV_MSG, 0, sizeof(g), port, 0, MACH_PORT_NULL);
	if (kr != KERN_SUCCESS) {
		printf("cow_test: %s: receive failed %#x\n", what, kr);
		return;
	}
	{
		const unsigned char *got = g.message.data.address;
		uintptr_t start = (uintptr_t)got;

		for (size_t at = 0; at < size;) {
			size_t next = ((start + at) & ~(page - 1)) + page - start, end = next < size ? next : size, bad = 0;

			for (size_t i = at; i < end; i++) {
				bad += got[i] != 0x11;
			}
			if (bad != 0 && used < sizeof(line) - 40) {
				used += (size_t)snprintf(line + used, sizeof(line) - used, " page %zu (bytes %zu-%zu): %zu", pages, at, end, bad);
			}
			changed += bad != 0;
			pages++;
			at = end;
		}
		printf("cow_test: %s (%zu bytes, %zu into its page, received %zu into its page): %zu of %zu pages show what was "
		    "written after the send%s%s\n", what, size, (size_t)((uintptr_t)buffer & (page - 1)), (size_t)(start & (page - 1)),
		    changed, pages, changed ? ":" : "", line);
		vm_deallocate(mach_task_self(), (vm_address_t)got, size);
	}
	mach_port_destroy(mach_task_self(), port);
}

/* vm_copy() into memory that is there already (what a picture's copy-on-write snapshot may be made with) */
static void
two(const char *what, unsigned char *from, unsigned char *to, size_t size, int write_after)
{
	size_t page = (size_t)getpagesize(), pages = 0, bad_pages = 0;
	char line[512] = "";
	size_t used = 0;
	kern_return_t kr;

	memset(from, 0x11, size);
	memset(to, 0x44, size);
	kr = vm_copy(mach_task_self(), (vm_address_t)from, size, (vm_address_t)to);
	if (write_after == 1) {
		memset(from, 0x22, size);               /* the source written after the copy */
	} else if (write_after == 2) {
		memset(to + size / 2, 0x55, 1);         /* the copy written in one place */
	}
	for (size_t at = 0; at < size;) {
		size_t next = (((uintptr_t)to + at) & ~(page - 1)) + page - (uintptr_t)to, end = next < size ? next : size, bad = 0;

		for (size_t i = at; i < end; i++) {
			bad += to[i] != 0x11 && !(write_after == 2 && i == size / 2);
		}
		if (bad != 0 && used < sizeof(line) - 40) {
			used += (size_t)snprintf(line + used, sizeof(line) - used, " page %zu (bytes %zu-%zu): %zu", pages, at, end, bad);
		}
		bad_pages += bad != 0;
		pages++;
		at = end;
	}
	if (write_after == 2) {
		size_t leaked = 0;

		for (size_t i = 0; i < size; i++) {
			leaked += from[i] != 0x11;
		}
		used += (size_t)snprintf(line + used, sizeof(line) - used, "; the source changed in %zu bytes", leaked);
	}
	printf("cow_test: vm_copy %s (%zu bytes, from %zu into its page to %zu into its): %#x; %zu of %zu pages of the copy wrong%s\n",
	    what, size, (size_t)((uintptr_t)from & (page - 1)), (size_t)((uintptr_t)to & (page - 1)), kr, bad_pages, pages, line);
}

/* sent out of line, received, the copy wired (as a surface around it would be), and only then the source written */
static void
three(const char *what, unsigned char *buffer, size_t size, int surface_mode)
{
	IOSurfaceRef surface = NULL;
	mach_port_t port;
	struct sent m;
	struct got g;
	size_t page = (size_t)getpagesize(), changed = 0, pages = 0;
	char line[512] = "";
	size_t used = 0;
	int locked;

	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
	mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
	memset(buffer, 0x11, size);
	memset(&m, 0, sizeof(m));
	m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
	m.header.msgh_size = sizeof(m);
	m.header.msgh_remote_port = port;
	m.body.msgh_descriptor_count = 1;
	m.data.address = buffer;
	m.data.size = (mach_msg_size_t)size;
	m.data.copy = MACH_MSG_VIRTUAL_COPY;
	m.data.type = MACH_MSG_OOL_DESCRIPTOR;
	mach_msg(&m.header, MACH_SEND_MSG, sizeof(m), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
	memset(&g, 0, sizeof(g));
	mach_msg(&g.message.header, MACH_RCV_MSG, 0, sizeof(g), port, 0, MACH_PORT_NULL);
	{
		const unsigned char *got = g.message.data.address;
		uintptr_t start = (uintptr_t)got;

		vm_protect(mach_task_self(), (vm_address_t)got & ~(page - 1), size + page, 0, VM_PROT_READ);
		if (!surface_mode) {
			locked = mlock((const void *)((uintptr_t)got & ~(page - 1)), ((start + size + page - 1) & ~(page - 1)) - (start & ~(page - 1)));
		} else {
			/* a surface made around the received copy, as CoreAnimation's window server does with a picture */
			CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			long long address = (long long)start, w = 180, h = 180, row = 720, bpe = 4, alloc = (long long)size, format = 0;
			struct { CFStringRef key; long long *value; } values[] = { { CFSTR("IOSurfaceAddress"), &address },
				{ CFSTR("IOSurfaceWidth"), &w }, { CFSTR("IOSurfaceHeight"), &h }, { CFSTR("IOSurfaceBytesPerRow"), &row },
				{ CFSTR("IOSurfaceBytesPerElement"), &bpe }, { CFSTR("IOSurfaceAllocSize"), &alloc },
				{ CFSTR("IOSurfacePixelFormat"), &format } };

			for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
				CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongLongType, values[i].value);

				CFDictionarySetValue(d, values[i].key, n);
				CFRelease(n);
			}
			surface = IOSurfaceCreate(d);
			locked = surface != NULL ? 0 : -1;
			CFRelease(d);
		}
		memset(buffer, 0x22, size);
		for (size_t at = 0; at < size;) {
			size_t next = ((start + at) & ~(page - 1)) + page - start, end = next < size ? next : size, bad = 0;

			for (size_t i = at; i < end; i++) {
				bad += got[i] != 0x11;
			}
			if (bad != 0 && used < sizeof(line) - 40) {
				used += (size_t)snprintf(line + used, sizeof(line) - used, " page %zu (bytes %zu-%zu): %zu", pages, at, end, bad);
			}
			changed += bad != 0;
			pages++;
			at = end;
		}
		{
			size_t source_bad = 0;

			for (size_t i = 0; i < size; i++) {
				source_bad += buffer[i] != 0x22;
			}
			printf("cow_test: %s (wired or surface made: %d, 0 is yes): %zu of %zu pages of the received copy show what was written after; the source "
			    "itself has %zu bytes that are not what was written to it%s\n", what, locked, changed, pages, source_bad, line);
		}
	}
	if (surface != NULL) {
		CFRelease(surface);
	}
	mach_port_destroy(mach_task_self(), port);
}

int
main(void)
{
	size_t size = 180 * 720, page = (size_t)getpagesize();
	vm_address_t region = 0;

	printf("cow_test: pages of %zu bytes\n", page);
	vm_allocate(mach_task_self(), &region, size + 4 * page, VM_FLAGS_ANYWHERE);
	one("a region of our own, page-aligned", (unsigned char *)region, size, 0);
	one("a region of our own, 4224 bytes in", (unsigned char *)region + 4224, size, 0);
	one("a region of our own, 4224 bytes in, neighbours written first", (unsigned char *)region + 4224, size, 1);
	{
		vm_address_t a = 0, b = 0;

		vm_allocate(mach_task_self(), &a, size + 4 * page, VM_FLAGS_ANYWHERE);
		vm_allocate(mach_task_self(), &b, size + 4 * page, VM_FLAGS_ANYWHERE);
		memset((void *)b, 0x66, size + 4 * page);       /* (the destination's pages in use already) */
		two("aligned", (unsigned char *)a, (unsigned char *)b, size, 0);
		two("aligned, source written after", (unsigned char *)a, (unsigned char *)b, size, 1);
		two("aligned, copy written after", (unsigned char *)a, (unsigned char *)b, size, 2);
		two("4224 in both", (unsigned char *)a + 4224, (unsigned char *)b + 4224, size, 0);
		two("4224 in both, source written after", (unsigned char *)a + 4224, (unsigned char *)b + 4224, size, 1);
		two("4224 in both, copy written after", (unsigned char *)a + 4224, (unsigned char *)b + 4224, size, 2);
		two("4224 and 2816", (unsigned char *)a + 4224, (unsigned char *)b + 2816, size, 1);
		three("received, made read-only and wired, then the source written (aligned)", (unsigned char *)a, size, 0);
		three("received, made read-only and wired, then the source written (4224 in)", (unsigned char *)a + 4224, size, 0);
		three("received, made read-only, a surface made around it, then the source written (aligned)", (unsigned char *)a, size, 1);
		three("received, made read-only, a surface made around it, then the source written (4224 in)", (unsigned char *)a + 4224, size, 1);
		{
			unsigned char *heap = malloc(size);

			three("malloc, received, made read-only, a surface made around it, then the source written", heap, size, 1);
		}
		{
			vm_address_t p = 0, q = 0;
			kern_return_t kp = vm_allocate(mach_task_self(), &p, size + 4 * page, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE);
			kern_return_t kq = vm_allocate(mach_task_self(), &q, size + 4 * page, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE);

			printf("cow_test: purgeable regions: %#x %#x\n", kp, kq);
			two("purgeable source, aligned, source written after", (unsigned char *)p, (unsigned char *)b, size, 1);
			two("purgeable source, 4224 in both, source written after", (unsigned char *)p + 4224, (unsigned char *)b + 4224, size, 1);
			two("purgeable both, 4224 in both, source written after", (unsigned char *)p + 4224, (unsigned char *)q + 4224, size, 1);
			two("purgeable destination, 4224 in both, source written after", (unsigned char *)a + 4224, (unsigned char *)q + 4224, size, 1);
			one("OOL from a purgeable region, 4224 bytes in", (unsigned char *)p + 4224, size, 0);
			three("OOL from a purgeable region, received, a surface made around it, then the source written (4224 in)",
			    (unsigned char *)p + 4224, size, 1);
		}
		printf("cow_test: the source locked in memory (wired): mlock %d\n", mlock((void *)a, size + 4 * page));
		two("wired source, aligned, source written after", (unsigned char *)a, (unsigned char *)b, size, 1);
		two("wired source, 4224 in both, source written after", (unsigned char *)a + 4224, (unsigned char *)b + 4224, size, 1);
		one("OOL from a wired region, 4224 bytes in", (unsigned char *)a + 4224, size, 0);
		munlock((void *)a, size + 4 * page);
		printf("cow_test: the destination locked in memory (wired): mlock %d\n", mlock((void *)b, size + 4 * page));
		two("wired destination, 4224 in both, source written after", (unsigned char *)a + 4224, (unsigned char *)b + 4224, size, 1);
		munlock((void *)b, size + 4 * page);
	}
	{
		unsigned char *heap = malloc(size);

		one("malloc", heap, size, 0);
		one("malloc, neighbours written first", heap, size, 1);
	}
	return 0;
}
