/*
 * The whole of what the Wi-Fi driver's object asks of the kernel it is linked into. That object
 * (tools/mk-ath11k.sh in the repository: Linux's ath11k on FreeBSD's LinuxKPI headers and our
 * wifi/lx) is compiled as FreeBSD kernel code, bsd/dev/redwood_lx.c on this side as XNU's;
 * neither sees the other's headers, so nothing but C's own types crosses. Everything the
 * driver's side knows of locks, timers, work queues and buffers is built in wifi/lx/src on the
 * few calls here.
 *
 * Threads: the driver's code runs on threads made by lx_os_thread() and on the kernel's own
 * threads that call into it; never in an interrupt.
 */
#ifndef _SYS_REDWOOD_LX_H_
#define _SYS_REDWOOD_LX_H_

/* ---- memory ---- */
void	*lx_os_alloc(unsigned long size);			/* zeroed; NULL when there is none */
void	 lx_os_free(void *memory, unsigned long size);
/*
 * Memory the radio itself reads and writes: whole pages, zeroed, which the radio knows from
 * *address on (the machine says where: the radio sees memory through an SMMU). What either
 * side writes the other sees at once: the processor's cache is not in between, which makes
 * reading it slow. NULL when there is none; never given back.
 */
void	*lx_os_radio_memory(unsigned long size, unsigned long long *address);
void	*lx_os_map_registers(unsigned long long physical, unsigned long size);	/* never unmapped */

/* ---- saying things ---- */
void	 lx_os_print(const char *text);
void	 lx_os_panic(const char *text) __attribute__((noreturn));

/* ---- time, threads, sleeping ---- */
unsigned long long lx_os_nanoseconds(void);			/* since some moment; never goes back */
void	*lx_os_thread_self(void);				/* a value that tells the kernel's threads apart */
/* a thread that runs function(argument) and ends when that returns: 0, or an errno */
int	 lx_os_thread(const char *name, void (*function)(void *), void *argument);
/*
 * One lock, and sleeping under it as with a condition variable: lx_os_sleep() is called with the
 * lock held, gives it up while asleep and has it again when it returns -- 0 when woken by
 * lx_os_wakeup() on the same address, 1 when the time (nanoseconds; 0: no limit) ran out.
 * The lock is held for a few instructions at a time and never across a call out of the layer.
 */
void	 lx_os_lock(void);
void	 lx_os_unlock(void);
int	 lx_os_sleep(const void *address, unsigned long long nanoseconds);
void	 lx_os_wakeup(const void *address);			/* everyone asleep on it */

/* ---- the rest of the machine ---- */
void	 lx_os_random(void *buffer, unsigned long size);	/* bytes nobody can guess */
/* a number given to the kernel when it was started ("wifi_lx=2"); 0 if it was not */
unsigned lx_os_option(const char *name);
/* a file the driver asks for by the name Linux has for it, in place: 0, or an errno (ENOENT) */
int	 lx_os_firmware(const char *name, const void **data, unsigned long *size);
/* the processor that runs the radio's firmware: started, and up when this returns; 0 or an errno */
int	 lx_os_rproc_boot(void);

/*
 * ---- the one name the object shows the kernel: bring the driver up on the radio and go as far
 * as what is built goes. 0, or an errno. Returns when that is over, or has failed.
 */
int	 lx_start(void);

#endif
