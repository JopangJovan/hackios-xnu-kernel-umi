/*
 * /dev/redwood-touch: touch samples on their way from whoever produces them to the
 * program that turns them into HID events for the window server.
 *
 * iOS expects touches from Apple's multitouch driver through IOHIDFamily; neither exists
 * here. Instead a producer -- a kernel driver for a real touch controller or for an emulated
 * pointer, or a user program replaying a script -- posts samples, and code of ours running
 * inside backboardd reads them and synthesizes the events. Both sides use the same record.
 *
 *   write(): one or more records (time is ignored; the kernel stamps them)
 *   read():  blocks until there is at least one record, returns as many whole ones as fit
 */
#ifndef _SYS_REDWOOD_INPUT_H_
#define _SYS_REDWOOD_INPUT_H_

#include <stdint.h>

#define REDWOOD_TOUCH_DEVICE    "/dev/redwood-touch"

#define REDWOOD_TOUCH_DOWN      1       /* a finger has landed */
#define REDWOOD_TOUCH_MOVE      2
#define REDWOOD_TOUCH_UP        3       /* it has lifted (x, y: where it was last) */
#define REDWOOD_BUTTON_DOWN     4       /* a hardware button; `finger` says which (REDWOOD_BUTTON_*), x and y are 0 */
#define REDWOOD_BUTTON_UP       5

#define REDWOOD_BUTTON_LOCK     1       /* the side button */
#define REDWOOD_BUTTON_VOLUME_UP 2
#define REDWOOD_BUTTON_VOLUME_DOWN 3
#define REDWOOD_BUTTON_HOME     4

struct redwood_touch {
	uint64_t time;                  /* mach_absolute_time() when the sample was posted */
	uint32_t type;                  /* REDWOOD_TOUCH_* or REDWOOD_BUTTON_DOWN/UP */
	uint32_t finger;                /* which finger, from 0 (or which button) */
	uint32_t x, y;                  /* position on the display, 0 = left/top edge, 65535 = right/bottom edge */
};

#ifdef KERNEL
/* For drivers; not from interrupt context. */
void redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
void redwood_input_init(void);
void redwood_virtio_input_init(void);   /* boot-arg virtio_input=1: QEMU's virtio pointer and keyboard */
void redwood_goodix_init(void);         /* boot-arg touchscreen=1: the phone's touch screen */
void redwood_buttons_init(void);        /* boot-arg buttons=1: the phone's power and volume keys */
void redwood_net_init(void);            /* en0, if there is something to carry its frames: sys/redwood_net.h */
void redwood_cpufreq_init(void);        /* boot-arg cpufreq=1: the phone's processor at full speed */
void redwood_adreno_init(void);         /* boot-arg adreno=N: the first N steps of waking the phone's GPU */
void redwood_adreno_quiesce(void);      /* on the way back to the loader: the GPU block stopped, out of our memory */
void redwood_adreno_stop(void);         /* before that, where the ending is no panic's: the GPU given back in order */
void redwood_adreno_minute(void);       /* once a minute: a line about how warm the GPU is, if it is in use */
void redwood_wifi_init(void);           /* boot-arg wifi=N: the first N steps towards the phone's Wi-Fi */
void redwood_wifi_stop(void);           /* where a run ends in order: the Wi-Fi DSP, if it was started, shut down */

/*
 * The GPU kept on (boot-arg adreno_keep=1), for iokit/Kernel/RedwoodGPU.cpp: see "the GPU kept on"
 * in bsd/dev/redwood_adreno.c. One caller at a time.
 */
bool redwood_gpu_ready(void);
bool redwood_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t redwood_gpu_address(uint64_t size);
void redwood_gpu_address_free(uint64_t address, uint64_t size);
bool redwood_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void redwood_gpu_mapped(void);
void redwood_gpu_unmap(uint64_t address, uint64_t size);
void redwood_gpu_to_memory(void *start, uint64_t size);
uint32_t redwood_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool redwood_gpu_done(uint32_t fence);
uint32_t redwood_gpu_last(void);
void redwood_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
void redwood_gpu_report(const char *why);
void redwood_gpu_stop(void);
#endif

#endif /* _SYS_REDWOOD_INPUT_H_ */
