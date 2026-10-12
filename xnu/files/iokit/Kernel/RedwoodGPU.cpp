/*
 * RedwoodGPU: the phone's Adreno for programs to draw with.
 *
 * bsd/dev/redwood_adreno.c wakes the GPU and, with boot-arg adreno_keep=1, keeps it on and
 * starts this service. A program that opens it speaks the interface of Linux's msm DRM driver,
 * the one Mesa's freedreno knows: every request of that interface is a call of the user client
 * with the request's number as the selector and the request's structure passed in and back
 * (userland: tools/mesa/redwood-drm, which stands in for libdrm inside Mesa). What Linux does
 * with mmap() on the device is the client memory of the type that is the buffer's handle.
 *
 * What there is of the interface is what freedreno uses to draw: parameters, buffers (made,
 * told their address in the GPU's address space, mapped, given back), submission of command
 * buffers, and waiting for one to be done. A buffer is wired kernel memory entered into the 3D
 * core's SMMU context; it is zero when made. The program has this processor's cache between
 * itself and the memory, the GPU has not: see submit().
 *
 * Not here yet: interrupts (waiting is asking again every millisecond), more than one program's
 * worth of care (every client's buffers are in the one address space; a client that goes takes
 * its own away), recovery of a GPU that has stopped answering (it is reported, see
 * redwood_gpu_report(), and stays stopped), fences and buffers as file descriptors.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/systm.h>

extern "C" {
/* bsd/dev/redwood_adreno.c ("the GPU kept on"; declared in bsd/sys/redwood_input.h, which is not seen from here) */
bool redwood_gpu_ready(void);
bool redwood_gpu_parameter(uint32_t which, uint64_t * value);
uint64_t redwood_gpu_address(uint64_t size);
void redwood_gpu_address_free(uint64_t address, uint64_t size);
bool redwood_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void redwood_gpu_mapped(void);
void redwood_gpu_unmap(uint64_t address, uint64_t size);
void redwood_gpu_to_memory(void * start, uint64_t size);
uint32_t redwood_gpu_submit(const uint64_t * addresses, const uint32_t * words, uint32_t count);
bool redwood_gpu_done(uint32_t fence);
uint32_t redwood_gpu_last(void);
void redwood_gpu_report(const char * why);
void redwood_gpu_stop(void);

void RedwoodGPUStart(void);
void RedwoodGPUStop(void);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);
}

/* Linux's include/uapi/drm/msm_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kRedwoodShow            = 0x7f,         /* ours: pixels of the caller's onto the panel, to see a test's result */
};

struct msm_timespec {
	int64_t seconds, nanoseconds;           /* of the clock that goes on while the machine sleeps */
};
struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};
struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};
struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};
struct msm_gem_madvise {
	uint32_t handle, madv, retained;
};
struct msm_submitqueue {
	uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
	uint64_t data;
	uint32_t id, param, len, pad;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct redwood_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};

#define MSM_INFO_GET_OFFSET     0
#define MSM_INFO_GET_IOVA       1
#define MSM_INFO_SET_NAME       2
#define MSM_INFO_GET_FLAGS      5
#define MSM_BO_GPU_READONLY     0x00000002
#define MSM_PREP_NOSYNC         0x04
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000

#define GPU_BUFFERS             8192            /* a client's, at most */
#define GPU_BUFFER_MAX          (256u << 20)
#define GPU_SUBMIT_CMDS         128
#define GPU_SUBMIT_BOS          8192
#define GPU_PATIENCE_MS         5000            /* for a buffer or a submission that somebody waits for without a limit */

static IOLock * gGPULock;                       /* around everything that is the GPU's (redwood_gpu_...) */
static bool gGPUReported;

/* Wait for the 3D core to be done with submission `fence`; with the lock held, which is let go of while waiting. */
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
	for (uint64_t waited = 0;; waited++) {
		if (redwood_gpu_done(fence)) {
			return true;
		}
		if (waited >= milliseconds) {
			break;
		}
		IOLockUnlock(gGPULock);
		IOSleep(1);
		IOLockLock(gGPULock);
	}
	if (milliseconds >= 1000 && !gGPUReported) {
		gGPUReported = true;
		redwood_gpu_report("no answer");
	}
	return false;
}

/* How long from now a time of msm's is, in milliseconds; 0: now or past. */
static uint64_t
millisecondsUntil(const struct msm_timespec * when)
{
	uint64_t now;
	int64_t left;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	left = when->seconds * (int64_t) NSEC_PER_SEC + when->nanoseconds - (int64_t) now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t) left / NSEC_PER_MSEC > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : (uint64_t) left / NSEC_PER_MSEC;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class RedwoodGPU : public IOService
{
	OSDeclareDefaultStructors(RedwoodGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodGPU, IOService);

bool
RedwoodGPU::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "RedwoodGPUUserClient");
	setName("RedwoodGPU");
	registerService();
	return true;
}

void
RedwoodGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	RedwoodGPU * gpu;

	gGPULock = IOLockAlloc();
	gpu = OSTypeAlloc(RedwoodGPU);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("RedwoodGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("RedwoodGPU: the service did not start\n");
	}
	gpu->release();
}

/* The run is ending (redwood_adreno.c): nobody is in the middle of anything when the GPU goes. */
void
RedwoodGPUStop(void)
{
	IOLockLock(gGPULock);
	redwood_gpu_stop();
	IOLockUnlock(gGPULock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct gpu_buffer {
	IOBufferMemoryDescriptor *      memory;         /* NULL: this handle is free */
	uint64_t                        address;        /* the GPU's for it */
	uint64_t                        size;
	uint32_t                        flags;
};

class RedwoodGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(RedwoodGPUUserClient);

	task_t                  fTask;
	struct gpu_buffer *     fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextQueue;
	uint32_t                fBufferCount;
	uint64_t                fBufferBytes;
	uint32_t                fSubmits;

	struct gpu_buffer * buffer(uint32_t handle);
	void freeBuffer(struct gpu_buffer * b);
	IOReturn gemNew(struct msm_gem_new * request);
	IOReturn gemInfo(struct msm_gem_info * request);
	IOReturn submit(struct msm_gem_submit * request);
	IOReturn show(const struct redwood_show * request);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodGPUUserClient, IOUserClient);

bool
RedwoodGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
RedwoodGPUUserClient::start(IOService * provider)
{
	if (!redwood_gpu_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
	return fBuffers != NULL;
}

/* With the lock held. */
void
RedwoodGPUUserClient::freeBuffer(struct gpu_buffer * b)
{
	if (redwood_gpu_ready()) {
		redwood_gpu_unmap(b->address, b->size);
		redwood_gpu_address_free(b->address, b->size);
	}
	b->memory->release();
	b->memory = NULL;
	fBufferCount--;
	fBufferBytes -= b->size;
}

IOReturn
RedwoodGPUUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		IOLockLock(gGPULock);
		/* what the client had the GPU do last may still be going on, in these buffers */
		if (redwood_gpu_ready()) {
			gpuWait(redwood_gpu_last(), 1000);
		}
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		IOLockUnlock(gGPULock);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
RedwoodGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

struct gpu_buffer *
RedwoodGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		return NULL;
	}
	return &fBuffers[handle - 1];
}

IOReturn
RedwoodGPUUserClient::gemNew(struct msm_gem_new * request)
{
	uint64_t size = round_page_64(request->size);
	struct gpu_buffer * b = NULL;
	IOBufferMemoryDescriptor * memory;
	bool mapped = true;

	if (size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
	    (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("RedwoodGPU: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size, fBufferCount,
		    fBufferBytes >> 20);
		return kIOReturnNoMemory;
	}
	bzero(memory->getBytesNoCopy(), size);
	redwood_gpu_to_memory(memory->getBytesNoCopy(), size);

	IOLockLock(gGPULock);
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b == NULL || (b->address = redwood_gpu_address(size)) == 0) {
		IOLockUnlock(gGPULock);
		memory->release();
		IOLog("RedwoodGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
		return kIOReturnNoMemory;
	}
	for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && redwood_gpu_map(b->address + offset, phys, request->flags & MSM_BO_GPU_READONLY);
	}
	redwood_gpu_mapped();
	if (!mapped) {
		redwood_gpu_unmap(b->address, size);
		redwood_gpu_address_free(b->address, size);
		IOLockUnlock(gGPULock);
		memory->release();
		IOLog("RedwoodGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	b->size = size;
	b->flags = request->flags;
	fBufferCount++;
	fBufferBytes += size;
	IOLockUnlock(gGPULock);
	return kIOReturnSuccess;
}

IOReturn
RedwoodGPUUserClient::gemInfo(struct msm_gem_info * request)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	IOLockLock(gGPULock);
	b = buffer(request->handle);
	if (b == NULL) {
		ret = kIOReturnBadArgument;
	} else if (request->info == MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t) request->handle << 32;      /* nobody maps by it here */
	} else if (request->info == MSM_INFO_GET_IOVA) {
		request->value = b->address;
	} else if (request->info == MSM_INFO_GET_FLAGS) {
		request->value = b->flags;
	} else if (request->info != MSM_INFO_SET_NAME) {            /* a name for somebody's debugger: not kept */
		ret = kIOReturnUnsupported;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
RedwoodGPUUserClient::submit(struct msm_gem_submit * request)
{
	struct msm_gem_submit_bo * bos = NULL;
	struct msm_gem_submit_cmd * cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0;
	IOReturn ret = kIOReturnBadArgument;

	if (request->nr_cmds == 0 || request->nr_cmds > GPU_SUBMIT_CMDS || request->nr_bos == 0 || request->nr_bos > GPU_SUBMIT_BOS) {
		IOLog("RedwoodGPU: a submission of %u command buffers in %u buffers: not taken\n", request->nr_cmds, request->nr_bos);
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, request->nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, request->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t) request->bos, bos, request->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t) request->cmds, cmds, request->nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	IOLockLock(gGPULock);
	/*
	 * The program writes its buffers through this processor's cache and the GPU reads memory,
	 * and the other way round for what the GPU draws: everything the submission names goes out
	 * of the cache now. (Mapped write-combined, as on Linux, none of this would be needed: see
	 * redwood_drm_map() in tools/mesa/redwood-drm.)
	 */
	for (uint32_t i = 0; i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b != NULL) {
			redwood_gpu_to_memory(b->memory->getBytesNoCopy(), b->size);
		}
	}
	for (uint32_t i = 0; i < request->nr_cmds; i++) {
		struct gpu_buffer * b = cmds[i].submit_idx < request->nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                               /* the other kinds are for a GPU shared between address spaces */
		}
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t) cmds[i].submit_offset + cmds[i].size > b->size) {
			IOLog("RedwoodGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
			    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			IOLockUnlock(gGPULock);
			goto out;
		}
		addresses[count] = b->address + cmds[i].submit_offset;
		words[count++] = cmds[i].size / 4;
	}
	request->fence = redwood_gpu_submit(addresses, words, count);
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	if (request->fence == 0 && !gGPUReported) {
		gGPUReported = true;
		redwood_gpu_report("the ring is full");
	}
	if (fSubmits++ < 4) {
		IOLog("RedwoodGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    addresses[0], words[0]);
	}
	IOLockUnlock(gGPULock);
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, request->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, request->nr_cmds);
	}
	return ret;
}

/* Pixels of the caller's (32 bits each, blue first) copied to the panel: for a test to show what it has drawn. */
IOReturn
RedwoodGPUUserClient::show(const struct redwood_show * request)
{
	uint64_t size = (uint64_t) request->rowBytes * request->height;
	uint8_t * pixels;
	IOReturn ret = kIOReturnSuccess;

	if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
	    request->rowBytes < 4 * request->width) {
		return kIOReturnBadArgument;
	}
	pixels = (uint8_t *) IOMallocData(size);
	if (pixels == NULL) {
		return kIOReturnNoMemory;
	}
	if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
		ret = kIOReturnVMError;
	} else {
		MiniIOMobileFramebufferShow(pixels, request->width, request->height, request->rowBytes);
	}
	IOFreeData(pixels, size);
	return ret;
}

IOReturn
RedwoodGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gGPULock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
RedwoodGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct msm_param param;
		struct msm_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
		struct redwood_show show;
	} r;
	size_t size = args->structureInputSize;
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!redwood_gpu_ready() && selector != kRedwoodShow) {
		return kIOReturnNotReady;                       /* given back: the run is ending */
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kMsmGetParam:
		ret = redwood_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kMsmSetParam:                              /* the program's name, for Linux's lists of who uses the GPU */
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
	case kMsmGemCpuPrep:                            /* is the GPU done with a buffer: with all it has been given, here */
		IOLockLock(gGPULock);
		if (!gpuWait(redwood_gpu_last(), (r.cpuPrep.op & MSM_PREP_NOSYNC) ? 0 : millisecondsUntil(&r.cpuPrep.timeout))) {
			ret = kIOReturnBusy;
		}
		IOLockUnlock(gGPULock);
		break;
	case kMsmGemSubmit:
		ret = submit(&r.submit);
		break;
	case kMsmWaitFence:
		IOLockLock(gGPULock);
		if (!gpuWait(r.waitFence.fence, millisecondsUntil(&r.waitFence.timeout))) {
			ret = kIOReturnTimeout;
		}
		IOLockUnlock(gGPULock);
		break;
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		r.queue.id = ++fNextQueue;
		break;
	case kMsmSubmitQueueQuery:                      /* how often the GPU has faulted on this client's commands */
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			copyout(&zero, (user_addr_t) r.query.data, sizeof(zero));
		}
		break;
	case kDrmGemClose:
		IOLockLock(gGPULock);
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			gpuWait(redwood_gpu_last(), 1000);
			freeBuffer(b);
		}
		IOLockUnlock(gGPULock);
		break;
	case kRedwoodShow:
		ret = show(&r.show);
		break;
	default:
		IOLog("RedwoodGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t) size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}
