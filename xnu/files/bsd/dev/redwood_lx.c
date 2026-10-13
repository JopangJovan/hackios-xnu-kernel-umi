/*
 * Darwin-on-redwood: the kernel's side of the Wi-Fi driver's object.
 *
 * The driver of the phone's radio is Linux's ath11k, compiled unmodified outside this tree on a
 * layer that gives it the kernel interfaces it expects, and tied into one object that the
 * kernel links when it is there (tools/mk-ath11k.sh and wifi/lx in the repository; the kernel
 * is built with that object when xnu-build.sh finds it). The object shows one name, lx_start(),
 * and asks for the calls of sys/redwood_lx.h, which are here: memory, time, threads, one lock
 * to sleep under, the kernel's options. What it asks of the radio itself -- its firmware's
 * files, its view of memory, the processor its firmware runs on -- is redwood_wifi.c's to
 * answer, which knows the phone.
 *
 * A kernel built without the object has the lx_start() at the end of this file, which says so.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/redwood_input.h>
#include <sys/redwood_lx.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern kern_return_t thread_terminate(thread_t thread);
extern void thread_set_thread_name(thread_t thread, const char *name);

#define say redwood_wifi_say

static LCK_GRP_DECLARE(lx_lock_group, "redwood-lx");
static LCK_MTX_DECLARE(lx_lock, &lx_lock_group);

/* ---- memory ------------------------------------------------------------------------------------------ */

void *
lx_os_alloc(unsigned long size)
{
	return kalloc_data(size, Z_WAITOK | Z_ZERO);
}

void
lx_os_free(void *memory, unsigned long size)
{
	kfree_data(memory, size);
}

/*
 * In one physical piece, so that it can be mapped a second time without this processor's cache
 * in between (as redwood_adreno.c does with what it shares with the GPU); the first, cached
 * view is written out and never used again.
 */
void *
lx_os_radio_memory(unsigned long size, unsigned long long *address)
{
	vm_offset_t cached = 0, uncached;
	uint64_t physical;

	size = round_page(size);
	if (kmem_alloc_contig(kernel_map, &cached, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		say("lx: no %lu bytes of memory in one piece\n", size);
		return NULL;
	}
	physical = kvtophys(cached);
	bzero((void *)cached, size);
	for (vm_offset_t line = cached; line < cached + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	uncached = ml_io_map_unmappable(physical, size, VM_WIMG_WCOMB);
	if (uncached == 0 || !redwood_wifi_radio_map(physical, size, address)) {
		return NULL;
	}
	return (void *)uncached;
}

void *
lx_os_map_registers(unsigned long long physical, unsigned long size)
{
	unsigned long long page = trunc_page(physical);
	vm_offset_t mapped = ml_io_map_unmappable(page, round_page(physical + size) - page, VM_WIMG_IO);

	return mapped == 0 ? NULL : (void *)(mapped + (physical - page));
}

/* ---- saying things ----------------------------------------------------------------------------------- */

void
lx_os_print(const char *text)
{
	say("%s", text);
}

void
lx_os_panic(const char *text)
{
	panic("%s", text);
}

/* ---- time, threads, sleeping --------------------------------------------------------------------------- */

unsigned long long
lx_os_nanoseconds(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds;
}

void *
lx_os_thread_self(void)
{
	return current_thread();
}

struct lx_thread {
	void (*function)(void *);
	void *argument;
};

static void
lx_thread_runs(void *parameter, __unused wait_result_t result)
{
	struct lx_thread start = *(struct lx_thread *)parameter;

	kfree_type(struct lx_thread, parameter);
	start.function(start.argument);
	thread_terminate(current_thread());
}

int
lx_os_thread(const char *name, void (*function)(void *), void *argument)
{
	struct lx_thread *start = kalloc_type(struct lx_thread, Z_WAITOK | Z_ZERO);
	thread_t thread;

	if (start == NULL) {
		return ENOMEM;
	}
	start->function = function;
	start->argument = argument;
	if (kernel_thread_start(lx_thread_runs, start, &thread) != KERN_SUCCESS) {
		kfree_type(struct lx_thread, start);
		return EAGAIN;
	}
	thread_set_thread_name(thread, name);
	thread_deallocate(thread);
	return 0;
}

void
lx_os_lock(void)
{
	lck_mtx_lock(&lx_lock);
}

void
lx_os_unlock(void)
{
	lck_mtx_unlock(&lx_lock);
}

int
lx_os_sleep(const void *address, unsigned long long nanoseconds)
{
	uint64_t deadline;

	if (nanoseconds == 0) {
		return lck_mtx_sleep(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT) == THREAD_TIMED_OUT;
	}
	nanoseconds_to_absolutetime(nanoseconds, &deadline);
	deadline += mach_absolute_time();
	return lck_mtx_sleep_deadline(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT, deadline) == THREAD_TIMED_OUT;
}

void
lx_os_wakeup(const void *address)
{
	thread_wakeup((event_t)address);
}

/* ---- the rest of the machine ------------------------------------------------------------------------- */

unsigned
lx_os_option(const char *name)
{
	uint32_t value = 0;

	PE_parse_boot_argn(name, &value, sizeof(value));
	return value;
}

int
lx_os_firmware(const char *name, const void **data, unsigned long *size)
{
	uint64_t length = 0;
	const uint8_t *file = redwood_wifi_driver_file(name, &length);

	if (file == NULL) {
		return ENOENT;
	}
	*data = file;
	*size = (unsigned long)length;
	return 0;
}

int
lx_os_rproc_boot(void)
{
	return redwood_wifi_dsp_is_up() ? 0 : ENXIO;
}

/* ---- the driver ---------------------------------------------------------------------------------------- */

/* The object has its own; this is the one of a kernel built without it. */
__attribute__((weak)) int
lx_start(void)
{
	say("lx: THE DRIVER IS NOT BUILT IN: its object was not there when this kernel was built (tools/mk-ath11k.sh, then "
	    "build the kernel again)\n");
	return ENXIO;
}

int
redwood_lx_start(void)
{
	return lx_start();
}
