/*
 * MiniIOSurface: a stand-in for Apple's closed IOSurface.kext (see MiniIOSurface.h).
 *
 * A surface is a piece of wired kernel memory: one page of state that clients read directly,
 * followed by the pixels. Every client that knows a surface has the whole of it mapped once.
 * Surfaces travel between processes as Mach send rights (IOSurfaceSendRight) or by number.
 *
 * What is not here: child surfaces, wrapping of client memory, purgeability, compression,
 * notifications. Calls that are not implemented are logged once and refused.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSUnserialize.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include "MiniIOSurface.h"

static_assert(sizeof(struct minisurf_plane) == 0x80, "plane layout");
static_assert(sizeof(struct minisurf_result) == 0x68, "result layout");
static_assert(offsetof(struct minisurf_shared, attachments) == 0x30, "shared layout");
static_assert(sizeof(struct minisurf_result) + 20 * sizeof(struct minisurf_plane) == MINISURF_RESULT_MAX, "result size");

/* IOConnectCallMethod selectors of IOSurfaceRootUserClient (iOS 16.5) */
enum {
	kSurfaceMethodCreate            = 0,
	kSurfaceMethodLookup            = 4,
	kSurfaceMethodSetYCbCrMatrix    = 5,
	kSurfaceMethodWrapClientImage   = 6,
	kSurfaceMethodWrapClientMemory  = 7,
	kSurfaceMethodSetValue          = 9,
	kSurfaceMethodCopyValue         = 10,
	kSurfaceMethodRemoveValue       = 11,
	kSurfaceMethodBindAccel         = 12,
	kSurfaceMethodInit              = 13,
	kSurfaceMethodLog               = 19,
	kSurfaceMethodSetPurgeable      = 20,
	kSurfaceMethodSetOwnership      = 21,
	kSurfaceMethodSetTiled          = 22,
	kSurfaceMethodSetTimestamp      = 24,
	kSurfaceMethodSetBulkAttachments = 27,
	kSurfaceMethodCommPageAddress   = 32,
	kSurfaceMethodSetIndexedTimestamp = 33,
	kSurfaceMethodLookupFromPort    = 34,
	kSurfaceMethodCreatePort        = 35,
	kSurfaceMethodDetachModeCode    = 43,
	kSurfaceMethodSetOwnershipIdentity = 44,
};

/* IOConnectTrap indices */
enum {
	kSurfaceTrapIncrementUseCount   = 0,
	kSurfaceTrapDecrementUseCount   = 1,
	kSurfaceTrapLock                = 2,
	kSurfaceTrapUnlock              = 3,
	kSurfaceTrapDetach              = 4,    /* the client is about to release: forget it has the surface */
	kSurfaceTrapRelease             = 5,
	kSurfaceTrapCount
};

#define kSurfaceLockReadOnly    1u

#define MINISURF_TABLE_SIZE     16384
#define MINISURF_MAX_DIMENSION  16384
#define MINISURF_MAX_ALLOC      (512u << 20)
#define MINISURF_ROW_ALIGN      64

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static IOLock *         gSurfaceLock;
static MiniIOSurface *  gSurfaceTable[MINISURF_TABLE_SIZE];     /* index = surface number; [0] unused */
static uint32_t         gSurfaceNext = 1;
static uint32_t         gSurfaceCount;
static uint64_t         gSurfaceBytes;
static uint64_t         gSurfaceLogged[2];                      /* selectors already complained about */
/* Boot-arg surftrace=N: log the first N surfaces created (default 24). */
static TUNABLE(uint32_t, gSurfaceTrace, "surftrace", 24);
static uint32_t         gSurfaceTraced;                         /* (a tunable is read-only after boot) */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSDefineMetaClassAndStructors(MiniIOSurface, OSObject);

void
MiniIOSurface::free(void)
{
	OSSafeReleaseNULL(values);
	OSSafeReleaseNULL(memory);
	OSObject::free();
}

/* The values changed: a new seed, and how much room a client needs to fetch them. */
void
MiniIOSurface::updateValuesState(void)
{
	struct minisurf_shared * sh = shared();
	uint32_t size = 0;

	if (values != NULL && values->getCount() != 0) {
		OSSerialize * s = OSSerialize::binaryWithCapacity(4096);
		if (s != NULL) {
			if (values->serialize(s)) {
				size = s->getLength() + 8;
			}
			s->release();
		}
	}
	sh->valuesSeed = (sh->valuesSeed + 2) | 1;
	sh->valuesSize = size;
}

static uint32_t
dictNumber(OSDictionary * dict, const char * key, uint32_t missing)
{
	OSObject * obj = dict ? dict->getObject(key) : NULL;
	OSNumber * num = OSDynamicCast(OSNumber, obj);

	if (num != NULL) {
		return num->unsigned32BitValue();
	}
	if (obj == kOSBooleanTrue) {
		return 1;
	}
	if (obj == kOSBooleanFalse) {
		return 0;
	}
	return missing;
}

static uint32_t
alignUp(uint64_t value, uint32_t to)
{
	return (uint32_t) ((value + to - 1) / to * to);
}

/* Bytes per element when the client did not say. */
static uint32_t
defaultBytesPerElement(uint32_t format)
{
	switch (format) {
	case FOURCC('L', '0', '0', '8'):
	case FOURCC('A', '0', '0', '8'):
	case 0x00000008:
		return 1;
	case FOURCC('L', '5', '6', '5'):
	case FOURCC('B', '5', '6', '5'):
	case FOURCC('L', '5', '5', '5'):
	case FOURCC('2', 'v', 'u', 'y'):
	case FOURCC('y', 'u', 'v', 's'):
	case FOURCC('y', 'u', 'v', 'f'):
	case FOURCC('L', '0', '1', '6'):
	case FOURCC('L', '0', '0', 'h'):
		return 2;
	case FOURCC('R', 'G', 'h', 'A'):
		return 8;
	case FOURCC('R', 'G', 'f', 'A'):
		return 16;
	default:
		return 4;
	}
}

/*
 * Two-plane YCbCr formats: luma, then interleaved chroma.
 * Returns false for anything else. *wide: 10-bit samples in 16 bits.
 */
static bool
biplanarLayout(uint32_t format, uint32_t * hsub, uint32_t * vsub, bool * wide)
{
	switch (format) {
	case FOURCC('4', '2', '0', 'v'): case FOURCC('4', '2', '0', 'f'):
		*hsub = 2; *vsub = 2; *wide = false; return true;
	case FOURCC('4', '2', '2', 'v'): case FOURCC('4', '2', '2', 'f'):
		*hsub = 2; *vsub = 1; *wide = false; return true;
	case FOURCC('4', '4', '4', 'v'): case FOURCC('4', '4', '4', 'f'):
		*hsub = 1; *vsub = 1; *wide = false; return true;
	case FOURCC('x', '4', '2', '0'): case FOURCC('x', 'f', '2', '0'):
		*hsub = 2; *vsub = 2; *wide = true; return true;
	case FOURCC('x', '4', '2', '2'): case FOURCC('x', 'f', '2', '2'):
		*hsub = 2; *vsub = 1; *wide = true; return true;
	case FOURCC('x', '4', '4', '4'): case FOURCC('x', 'f', '4', '4'):
		*hsub = 1; *vsub = 1; *wide = true; return true;
	default:
		return false;
	}
}

static void
describeComponents(struct minisurf_plane * plane, uint32_t format)
{
	/* in memory order; names: 1 alpha, 2 red, 3 green, 4 blue */
	static const uint8_t bgra[4] = { 4, 3, 2, 1 };
	static const uint8_t rgba[4] = { 2, 3, 4, 1 };
	const uint8_t * names;

	if (format == FOURCC('B', 'G', 'R', 'A')) {
		names = bgra;
	} else if (format == FOURCC('R', 'G', 'B', 'A')) {
		names = rgba;
	} else {
		return;
	}
	plane->componentCount = 4;
	for (unsigned i = 0; i < 4; i++) {
		plane->componentBitDepth[i] = 8;
		plane->componentBitOffset[i] = (uint8_t) (8 * i);
		plane->componentName[i] = names[i];
		plane->componentType[i] = 1;            /* unsigned integer */
		plane->componentRange[i] = 1;           /* full range */
	}
}

/*
 * Make a surface from the properties a client gave to IOSurfaceCreate().
 * Returned with one holder, and entered in the table.
 */
static MiniIOSurface *
surfaceCreate(OSDictionary * props, IOReturn * error)
{
	MiniIOSurface * s;
	struct minisurf_result * info;
	uint32_t width, height, format, bpe, ew, eh, bpr, alloc, offset, planeCount = 0;
	uint32_t hsub, vsub;
	bool wide;
	OSArray * planeInfo;
	uint64_t needed;

	*error = kIOReturnBadArgument;
	width = dictNumber(props, "IOSurfaceWidth", 0);
	height = dictNumber(props, "IOSurfaceHeight", 0);
	format = dictNumber(props, "IOSurfacePixelFormat", 0);
	/*
	 * Ten bits a colour, packed into 32-bit pixels: what a device with a wide-gamut display
	 * (the iPhone X is one) asks for when it wants a picture of the screen, a snapshot for a
	 * transition for instance. CoreAnimation's software renderer does not draw into those
	 * ("SW renderer invalid destination format w30r": the transition comes out black), and
	 * there is no wide gamut on this panel anyway. The surface is made as ordinary BGRA of
	 * the same geometry, and says so: whoever uses it reads the format back from the surface.
	 */
	if (format == 'w30r' || format == 'l10r' || format == 'w30R') {
		format = 'BGRA';
	}
	ew = dictNumber(props, "IOSurfaceElementWidth", 1);
	eh = dictNumber(props, "IOSurfaceElementHeight", 1);
	bpe = dictNumber(props, "IOSurfaceBytesPerElement", 0);
	offset = dictNumber(props, "IOSurfaceOffset", 0);
	if (width > MINISURF_MAX_DIMENSION || height > MINISURF_MAX_DIMENSION || ew == 0 || eh == 0 || ew > 255 || eh > 255) {
		return NULL;
	}
	if (props->getObject("IOSurfaceAddress") != NULL || props->getObject("IOSurfaceAddressRanges") != NULL) {
		IOLog("MiniIOSurface: wrapping client memory is not supported\n");
		*error = kIOReturnUnsupported;
		return NULL;
	}

	s = OSTypeAlloc(MiniIOSurface);
	if (s == NULL || !s->init()) {
		OSSafeReleaseNULL(s);
		*error = kIOReturnNoMemory;
		return NULL;
	}
	info = &s->info;

	planeInfo = OSDynamicCast(OSArray, props->getObject("IOSurfacePlaneInfo"));
	needed = 0;
	if (planeInfo != NULL && planeInfo->getCount() != 0) {
		planeCount = planeInfo->getCount();
		if (planeCount > MINISURF_MAX_PLANES) {
			IOLog("MiniIOSurface: %u planes asked for, %u supported\n", planeCount, MINISURF_MAX_PLANES);
			s->release();
			return NULL;
		}
		for (uint32_t i = 0; i < planeCount; i++) {
			OSDictionary * pd = OSDynamicCast(OSDictionary, planeInfo->getObject(i));
			struct minisurf_plane * p = &s->planes[i];
			uint32_t pew = dictNumber(pd, "IOSurfacePlaneElementWidth", 1);
			uint32_t peh = dictNumber(pd, "IOSurfacePlaneElementHeight", 1);

			if (pew == 0 || peh == 0 || pew > 255 || peh > 255) {
				s->release();
				return NULL;
			}
			p->width = dictNumber(pd, "IOSurfacePlaneWidth", width);
			p->height = dictNumber(pd, "IOSurfacePlaneHeight", height);
			p->bytesPerElement = (uint16_t) dictNumber(pd, "IOSurfacePlaneBytesPerElement", bpe ? bpe : 1);
			p->elementWidth = (uint8_t) pew;
			p->elementHeight = (uint8_t) peh;
			p->bytesPerRow = dictNumber(pd, "IOSurfacePlaneBytesPerRow", 0);
			if (p->bytesPerRow == 0) {
				p->bytesPerRow = alignUp((uint64_t) ((p->width + pew - 1) / pew) * p->bytesPerElement, MINISURF_ROW_ALIGN);
			}
			p->size = dictNumber(pd, "IOSurfacePlaneSize", 0);
			if (p->size == 0) {
				p->size = p->bytesPerRow * ((p->height + peh - 1) / peh);
			}
			p->offset = dictNumber(pd, "IOSurfacePlaneOffset", dictNumber(pd, "IOSurfacePlaneBase", (uint32_t) needed));
			p->extendedPixels[0] = dictNumber(pd, "IOSurfacePlaneExtendedPixelsOnLeft", 0);
			p->extendedPixels[1] = dictNumber(pd, "IOSurfacePlaneExtendedPixelsOnRight", 0);
			p->extendedPixels[2] = dictNumber(pd, "IOSurfacePlaneExtendedPixelsOnTop", 0);
			p->extendedPixels[3] = dictNumber(pd, "IOSurfacePlaneExtendedPixelsOnBottom", 0);
			if ((uint64_t) p->offset + p->size > needed) {
				needed = (uint64_t) p->offset + p->size;
			}
		}
	} else if (biplanarLayout(format, &hsub, &vsub, &wide) && width != 0 && height != 0) {
		struct minisurf_plane * y = &s->planes[0], * c = &s->planes[1];

		planeCount = 2;
		y->width = width;
		y->height = height;
		y->bytesPerElement = wide ? 2 : 1;
		y->elementWidth = y->elementHeight = 1;
		y->bytesPerRow = alignUp((uint64_t) width * y->bytesPerElement, MINISURF_ROW_ALIGN);
		y->size = y->bytesPerRow * height;
		y->offset = 0;
		c->width = (width + hsub - 1) / hsub;
		c->height = (height + vsub - 1) / vsub;
		c->bytesPerElement = wide ? 4 : 2;
		c->elementWidth = c->elementHeight = 1;
		c->bytesPerRow = alignUp((uint64_t) c->width * c->bytesPerElement, MINISURF_ROW_ALIGN);
		c->size = c->bytesPerRow * c->height;
		c->offset = y->size;
		needed = (uint64_t) c->offset + c->size;
	}

	if (bpe == 0) {
		bpe = planeCount ? s->planes[0].bytesPerElement : defaultBytesPerElement(format);
	}
	bpr = dictNumber(props, "IOSurfaceBytesPerRow", 0);
	if (bpr == 0) {
		bpr = planeCount ? s->planes[0].bytesPerRow
		    : alignUp((uint64_t) ((width + ew - 1) / ew) * bpe, MINISURF_ROW_ALIGN);
	}
	if (planeCount == 0) {
		needed = (uint64_t) bpr * ((height + eh - 1) / eh);
		/* the one implicit plane, for the per-plane getters */
		s->planes[0].width = width;
		s->planes[0].height = height;
		s->planes[0].bytesPerRow = bpr;
		s->planes[0].bytesPerElement = (uint16_t) bpe;
		s->planes[0].elementWidth = (uint8_t) ew;
		s->planes[0].elementHeight = (uint8_t) eh;
		s->planes[0].size = (uint32_t) needed;
		describeComponents(&s->planes[0], format);
	}
	needed += offset;
	alloc = dictNumber(props, "IOSurfaceAllocSize", 0);
	if (alloc < needed) {
		if (alloc != 0) {
			IOLog("MiniIOSurface: alloc size %u too small for %ux%u '%.4s', using %llu\n", alloc, width, height,
			    (const char *) &format, needed);
		}
		if (needed > MINISURF_MAX_ALLOC) {
			s->release();
			return NULL;
		}
		alloc = (uint32_t) needed;
	}
	if (alloc == 0 || alloc > MINISURF_MAX_ALLOC) {
		s->release();
		return NULL;
	}
	alloc = alignUp(alloc, (uint32_t) PAGE_SIZE);

	s->memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, (vm_size_t) PAGE_SIZE + alloc, PAGE_SIZE);
	if (s->memory == NULL) {
		IOLog("MiniIOSurface: no memory for %u bytes (%u surfaces, %llu MB in use)\n", alloc, gSurfaceCount,
		    gSurfaceBytes >> 20);
		s->release();
		*error = kIOReturnNoMemory;
		return NULL;
	}
	s->kbase = (uint8_t *) s->memory->getBytesNoCopy();
	s->values = OSDictionary::withCapacity(4);

	info->allocSize = alloc;
	info->width = width;
	info->height = height;
	info->bytesPerRow = bpr;
	info->offset = offset;
	info->pixelFormat = format;
	info->planeCount = planeCount;
	info->bytesPerElement = (uint16_t) bpe;
	info->elementWidth = (uint8_t) ew;
	info->elementHeight = (uint8_t) eh;
	info->cacheMode = dictNumber(props, "IOSurfaceCacheMode", 0);
	info->subsampling = (uint8_t) dictNumber(props, "IOSurfaceSubsampling", 0);
	info->prohibitUseCount = (uint8_t) dictNumber(props, "IOSurfaceProhibitUseCount", 0);
	info->sliceCount = dictNumber(props, "IOSurfaceSliceCount", 0);
	info->protectionOptions = dictNumber(props, "IOSurfaceProtectionOptions", 0);

	IOLockLock(gSurfaceLock);
	uint32_t sid = 0;
	for (uint32_t tries = 0; tries < MINISURF_TABLE_SIZE - 1; tries++) {
		uint32_t candidate = gSurfaceNext;

		gSurfaceNext = (gSurfaceNext + 1 < MINISURF_TABLE_SIZE) ? gSurfaceNext + 1 : 1;
		if (gSurfaceTable[candidate] == NULL) {
			sid = candidate;
			break;
		}
	}
	if (sid == 0) {
		IOLockUnlock(gSurfaceLock);
		IOLog("MiniIOSurface: the surface table is full\n");
		s->release();
		*error = kIOReturnNoResources;
		return NULL;
	}
	s->sid = sid;
	s->holders = 1;
	info->sid = sid;
	info->traceID = sid;
	gSurfaceTable[sid] = s;                 /* the table owns the object until the last holder goes */
	gSurfaceCount++;
	gSurfaceBytes += alloc;

	struct minisurf_shared * sh = s->shared();
	sh->registryID = 0x100000000ULL + sid;
	sh->seed = 1;
	sh->valuesSeed = 1;
	sh->pixelSizeCasting = (uint8_t) dictNumber(props, "IOSurfacePixelSizeCastingAllowed", 1);
	sh->displayable = 1;
	sh->sysMemOnly = (uint8_t) dictNumber(props, "IOSurfaceSysMemOnly", 0);
	IOLockUnlock(gSurfaceLock);

	*error = kIOReturnSuccess;
	return s;
}

/* With the lock held. */
static void
surfaceHold(MiniIOSurface * s)
{
	s->holders++;
}

/* With the lock held. The caller must not touch the surface afterwards. */
static void
surfaceUnhold(MiniIOSurface * s)
{
	if (--s->holders != 0) {
		return;
	}
	gSurfaceTable[s->sid] = NULL;
	gSurfaceCount--;
	gSurfaceBytes -= s->info.allocSize;
	s->release();
}

MiniIOSurface *
MiniIOSurfaceCopy(uint32_t sid)
{
	MiniIOSurface * s = NULL;

	if (gSurfaceLock == NULL || sid == 0 || sid >= MINISURF_TABLE_SIZE) {
		return NULL;
	}
	IOLockLock(gSurfaceLock);
	s = gSurfaceTable[sid];
	if (s != NULL) {
		surfaceHold(s);
	}
	IOLockUnlock(gSurfaceLock);
	return s;
}

void
MiniIOSurfaceDrop(MiniIOSurface * s)
{
	if (s == NULL) {
		return;
	}
	IOLockLock(gSurfaceLock);
	surfaceUnhold(s);
	IOLockUnlock(gSurfaceLock);
}

MiniIOSurface *
MiniIOSurfaceCreateSimple(uint32_t width, uint32_t height, uint32_t pixelFormat)
{
	OSDictionary * props = OSDictionary::withCapacity(4);
	MiniIOSurface * s = NULL;
	IOReturn error;

	if (props == NULL || gSurfaceLock == NULL) {
		OSSafeReleaseNULL(props);
		return NULL;
	}
	OSNumber * n;
	n = OSNumber::withNumber(width, 32); props->setObject("IOSurfaceWidth", n); n->release();
	n = OSNumber::withNumber(height, 32); props->setObject("IOSurfaceHeight", n); n->release();
	n = OSNumber::withNumber(pixelFormat, 32); props->setObject("IOSurfacePixelFormat", n); n->release();
	s = surfaceCreate(props, &error);
	props->release();
	return s;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* What a Mach send right for a surface points at. Holds the surface while any right exists. */
class IOSurfaceSendRight : public OSObject
{
	OSDeclareDefaultStructors(IOSurfaceSendRight);

public:
	MiniIOSurface * surface;

	virtual bool
	init(void) APPLE_KEXT_OVERRIDE
	{
		return OSObject::init();
	}
	virtual void free(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOSurfaceSendRight, OSObject);

void
IOSurfaceSendRight::free(void)
{
	MiniIOSurfaceDrop(surface);
	surface = NULL;
	OSObject::free();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOCoreSurfaceRoot : public IOService
{
	OSDeclareDefaultStructors(IOCoreSurfaceRoot);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);

bool
IOCoreSurfaceRoot::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");
	setName("IOCoreSurfaceRoot");
	registerService();
	return true;
}

void
MiniIOSurfaceStart(IOService * provider)
{
	IOCoreSurfaceRoot * root;

	gSurfaceLock = IOLockAlloc();
	root = OSTypeAlloc(IOCoreSurfaceRoot);
	if (root == NULL || !root->init() || !root->attach(provider)) {
		OSSafeReleaseNULL(root);
		IOLog("MiniIOSurface: cannot create the root service\n");
		return;
	}
	if (!root->start(provider)) {
		root->detach(provider);
		IOLog("MiniIOSurface: the root service did not start\n");
	}
	root->release();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* One surface as one client has it. */
struct minisurf_client_ref {
	MiniIOSurface * surface;                /* held */
	IOMemoryMap *   map;                    /* the whole of it in the client's address space */
	uint32_t        use[2];                 /* what this client added to the use counts */
	bool            detached;               /* the client let go of it and will release shortly */
};

class IOSurfaceRootUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOSurfaceRootUserClient);

	task_t                          fTask;
	struct minisurf_client_ref *    fRefs;
	uint32_t                        fRefCount;
	uint32_t                        fRefCapacity;
	IOExternalTrap                  fTraps[kSurfaceTrapCount];

	struct minisurf_client_ref * findRef(uint32_t sid, bool detached);
	struct minisurf_client_ref * addRef(MiniIOSurface * surface);
	void dropRef(struct minisurf_client_ref * ref);
	IOReturn returnSurface(struct minisurf_client_ref * ref, bool known, IOExternalMethodArguments * args);

	IOReturn methodInit(IOExternalMethodArguments * args);
	IOReturn methodCreate(IOExternalMethodArguments * args);
	IOReturn methodLookup(IOExternalMethodArguments * args);
	IOReturn methodLookupFromPort(IOExternalMethodArguments * args);
	IOReturn methodCreatePort(IOExternalMethodArguments * args);
	IOReturn methodSetValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize);
	IOReturn methodCopyValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize);
	IOReturn methodRemoveValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize);
	IOReturn methodSetBulkAttachments(const uint8_t * in, size_t inSize);

	IOReturn trapIncrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapDecrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapLock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapUnlock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapDetach(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn trapRelease(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6);
	IOReturn lockCommon(uint32_t sid, uint32_t options, user_addr_t seedOut);
	IOReturn useCountCommon(uint32_t sid, uint32_t category, int delta);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
	virtual IOExternalTrap * getTargetAndTrapForIndex(IOService ** targetP, UInt32 index) APPLE_KEXT_OVERRIDE;
	static mach_port_name_t makeSendRight(task_t task, MiniIOSurface * surface);
};

OSDefineMetaClassAndStructors(IOSurfaceRootUserClient, IOUserClient);

bool
IOSurfaceRootUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOSurfaceRootUserClient::start(IOService * provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fTraps[kSurfaceTrapIncrementUseCount].func = (IOTrap) & IOSurfaceRootUserClient::trapIncrementUseCount;
	fTraps[kSurfaceTrapDecrementUseCount].func = (IOTrap) & IOSurfaceRootUserClient::trapDecrementUseCount;
	fTraps[kSurfaceTrapLock].func = (IOTrap) & IOSurfaceRootUserClient::trapLock;
	fTraps[kSurfaceTrapUnlock].func = (IOTrap) & IOSurfaceRootUserClient::trapUnlock;
	fTraps[kSurfaceTrapDetach].func = (IOTrap) & IOSurfaceRootUserClient::trapDetach;
	fTraps[kSurfaceTrapRelease].func = (IOTrap) & IOSurfaceRootUserClient::trapRelease;
	for (unsigned i = 0; i < kSurfaceTrapCount; i++) {
		fTraps[i].object = this;
	}
	return true;
}

IOReturn
IOSurfaceRootUserClient::clientClose(void)
{
	IOLockLock(gSurfaceLock);
	while (fRefCount != 0) {
		dropRef(&fRefs[fRefCount - 1]);
	}
	IOLockUnlock(gSurfaceLock);
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOSurfaceRootUserClient::free(void)
{
	if (fRefs != NULL) {
		if (gSurfaceLock != NULL) {
			IOLockLock(gSurfaceLock);
			while (fRefCount != 0) {
				dropRef(&fRefs[fRefCount - 1]);
			}
			IOLockUnlock(gSurfaceLock);
		}
		IODelete(fRefs, struct minisurf_client_ref, fRefCapacity);
		fRefs = NULL;
	}
	IOUserClient::free();
}

/* All three with the lock held. */
struct minisurf_client_ref *
IOSurfaceRootUserClient::findRef(uint32_t sid, bool detached)
{
	for (uint32_t i = 0; i < fRefCount; i++) {
		if (fRefs[i].surface->sid == sid && fRefs[i].detached == detached) {
			return &fRefs[i];
		}
	}
	return NULL;
}

struct minisurf_client_ref *
IOSurfaceRootUserClient::addRef(MiniIOSurface * surface)
{
	struct minisurf_client_ref * ref;
	IOMemoryMap * map;

	if (fRefCount == fRefCapacity) {
		uint32_t capacity = fRefCapacity ? fRefCapacity * 2 : 16;
		struct minisurf_client_ref * refs = IONew(struct minisurf_client_ref, capacity);

		if (refs == NULL) {
			return NULL;
		}
		if (fRefs != NULL) {
			memcpy(refs, fRefs, fRefCount * sizeof(*refs));
			IODelete(fRefs, struct minisurf_client_ref, fRefCapacity);
		}
		fRefs = refs;
		fRefCapacity = capacity;
	}
	map = surface->memory->createMappingInTask(fTask, 0, kIOMapAnywhere);
	if (map == NULL) {
		IOLog("MiniIOSurface: cannot map surface %u (%u bytes) into the client\n", surface->sid, surface->info.allocSize);
		return NULL;
	}
	ref = &fRefs[fRefCount++];
	bzero(ref, sizeof(*ref));
	ref->surface = surface;
	ref->map = map;
	surfaceHold(surface);
	return ref;
}

void
IOSurfaceRootUserClient::dropRef(struct minisurf_client_ref * ref)
{
	MiniIOSurface * s = ref->surface;

	for (unsigned c = 0; c < 2; c++) {
		s->useCount[c] -= ref->use[c];
	}
	s->shared()->useCount = (uint64_t) s->useCount[1] << 32 | s->useCount[0];
	OSSafeReleaseNULL(ref->map);
	*ref = fRefs[--fRefCount];
	surfaceUnhold(s);
}

static uint32_t
resultSize(const MiniIOSurface * s)
{
	uint32_t planes = s->info.planeCount ? s->info.planeCount : 1;

	return (uint32_t) (sizeof(struct minisurf_result) + planes * sizeof(struct minisurf_plane));
}

/* Describe the surface to the client. known: it has the surface already, do not build a second view. */
IOReturn
IOSurfaceRootUserClient::returnSurface(struct minisurf_client_ref * ref, bool known, IOExternalMethodArguments * args)
{
	MiniIOSurface * s = ref->surface;
	uint32_t size = resultSize(s);
	struct minisurf_result * r = (struct minisurf_result *) args->structureOutput;
	mach_vm_address_t address = ref->map->getAddress();

	if (r == NULL || args->structureOutputSize < size) {
		return kIOReturnBadArgument;
	}
	*r = s->info;
	memcpy(r + 1, s->planes, size - sizeof(*r));
	r->shared = address;
	r->aux = address + MINISURF_AUX_OFFSET;
	r->base = known ? ~0ULL : address + PAGE_SIZE;
	args->structureOutputSize = size;
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodInit(IOExternalMethodArguments * args)
{
	struct {
		uint32_t allocAlignMask;
		uint32_t rowAlignMask;
		uint32_t maxBytesPerRow;
		uint32_t maxWidth;
		uint32_t maxHeight;
		uint16_t reserved;
		uint16_t version;
	} reply;

	static_assert(sizeof(reply) == 0x18, "init reply");
	if (args->structureOutput == NULL || args->structureOutputSize < sizeof(reply)) {
		return kIOReturnBadArgument;
	}
	reply.allocAlignMask = (uint32_t) PAGE_MASK;
	reply.rowAlignMask = MINISURF_ROW_ALIGN - 1;
	reply.maxBytesPerRow = MINISURF_MAX_DIMENSION * 16;
	reply.maxWidth = MINISURF_MAX_DIMENSION;
	reply.maxHeight = MINISURF_MAX_DIMENSION;
	reply.reserved = 0;
	reply.version = 1;
	memcpy(args->structureOutput, &reply, sizeof(reply));
	args->structureOutputSize = sizeof(reply);
	return kIOReturnSuccess;
}

/* The serialized object in a request, or NULL. */
static OSObject *
unserialize(const uint8_t * data, size_t size)
{
	if (data == NULL || size < 4) {
		return NULL;
	}
	return OSUnserializeXML((const char *) data, size, NULL);
}

IOReturn
IOSurfaceRootUserClient::methodCreate(IOExternalMethodArguments * args)
{
	IOReturn ret;
	IOMemoryMap * inMap = NULL;
	const uint8_t * in = (const uint8_t *) args->structureInput;
	size_t inSize = args->structureInputSize;
	OSObject * obj;
	OSDictionary * props;
	MiniIOSurface * s;
	struct minisurf_client_ref * ref;

	if (args->scalarInputCount >= 1 && args->scalarInput[0] != 0) {
		IOLog("MiniIOSurface: child surfaces are not supported (parent %llu)\n", args->scalarInput[0]);
		return kIOReturnUnsupported;
	}
	if (args->structureInputDescriptor != NULL) {
		inMap = args->structureInputDescriptor->map(kIOMapReadOnly);
		if (inMap == NULL) {
			return kIOReturnNoMemory;
		}
		in = (const uint8_t *) inMap->getVirtualAddress();
		inSize = inMap->getLength();
	}
	obj = unserialize(in, inSize);
	OSSafeReleaseNULL(inMap);
	props = OSDynamicCast(OSDictionary, obj);
	if (props == NULL) {
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	s = surfaceCreate(props, &ret);
	if (s == NULL) {
		IOLog("MiniIOSurface: create failed 0x%x: %ux%u format 0x%x alloc %u\n", ret,
		    dictNumber(props, "IOSurfaceWidth", 0), dictNumber(props, "IOSurfaceHeight", 0),
		    dictNumber(props, "IOSurfacePixelFormat", 0), dictNumber(props, "IOSurfaceAllocSize", 0));
		props->release();
		return ret;
	}
	props->release();
	if (gSurfaceTraced < gSurfaceTrace) {
		char procname[32] = "?";

		gSurfaceTraced++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOSurface: %s[%d] created surface %u: %ux%u '%c%c%c%c', %u bytes per row, %u planes, %u bytes\n",
		    procname, proc_selfpid(), s->sid, s->info.width, s->info.height, (int) (s->info.pixelFormat >> 24 & 0xff),
		    (int) (s->info.pixelFormat >> 16 & 0xff), (int) (s->info.pixelFormat >> 8 & 0xff),
		    (int) (s->info.pixelFormat & 0xff), s->info.bytesPerRow, s->info.planeCount, s->info.allocSize);
	}

	IOLockLock(gSurfaceLock);
	ref = addRef(s);
	ret = ref ? returnSurface(ref, false, args) : kIOReturnNoMemory;
	if (ref != NULL && ret != kIOReturnSuccess) {
		dropRef(ref);
	}
	surfaceUnhold(s);                       /* the creator's hold; the client's own is in the ref */
	IOLockUnlock(gSurfaceLock);
	return ret;
}

IOReturn
IOSurfaceRootUserClient::methodLookup(IOExternalMethodArguments * args)
{
	IOReturn ret = kIOReturnNotFound;
	uint32_t sid;
	struct minisurf_client_ref * ref;

	if (args->scalarInputCount < 1) {
		return kIOReturnBadArgument;
	}
	sid = (uint32_t) args->scalarInput[0];
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref == NULL && sid != 0 && sid < MINISURF_TABLE_SIZE && gSurfaceTable[sid] != NULL) {
		ref = addRef(gSurfaceTable[sid]);
		if (ref == NULL) {
			ret = kIOReturnNoMemory;
		}
	}
	if (ref != NULL) {
		ret = returnSurface(ref, false, args);
	}
	IOLockUnlock(gSurfaceLock);
	return ret;
}

IOReturn
IOSurfaceRootUserClient::methodLookupFromPort(IOExternalMethodArguments * args)
{
	IOReturn ret;
	OSObject * obj = NULL;
	IOSurfaceSendRight * right;
	struct minisurf_client_ref * ref;
	bool known;

	if (args->scalarInputCount < 1) {
		return kIOReturnBadArgument;
	}
	ret = copyObjectForPortNameInTask(fTask, (mach_port_name_t) args->scalarInput[0], &obj);
	right = OSDynamicCast(IOSurfaceSendRight, obj);
	if (ret != kIOReturnSuccess || right == NULL || right->surface == NULL) {
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(right->surface->sid, false);
	known = ref != NULL;
	if (ref == NULL) {
		ref = addRef(right->surface);
	}
	ret = ref ? returnSurface(ref, known, args) : kIOReturnNoMemory;
	IOLockUnlock(gSurfaceLock);
	obj->release();
	return ret;
}

mach_port_name_t
IOSurfaceRootUserClient::makeSendRight(task_t task, MiniIOSurface * s)
{
	IOSurfaceSendRight * right;
	mach_port_name_t name = 0;

	right = OSTypeAlloc(IOSurfaceSendRight);
	if (right == NULL || !right->init()) {
		OSSafeReleaseNULL(right);
		return 0;
	}
	IOLockLock(gSurfaceLock);
	surfaceHold(s);                         /* for as long as the send right exists */
	IOLockUnlock(gSurfaceLock);
	right->surface = s;
	copyPortNameForObjectInTask(task, right, &name);
	right->release();
	return name;
}

mach_port_name_t
MiniIOSurfaceMakeSendRight(task_t task, MiniIOSurface * surface)
{
	return IOSurfaceRootUserClient::makeSendRight(task, surface);
}

IOReturn
IOSurfaceRootUserClient::methodCreatePort(IOExternalMethodArguments * args)
{
	MiniIOSurface * s;
	mach_port_name_t name;

	if (args->scalarInputCount < 1 || args->scalarOutputCount < 1) {
		return kIOReturnBadArgument;
	}
	s = MiniIOSurfaceCopy((uint32_t) args->scalarInput[0]);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	name = makeSendRight(fTask, s);
	MiniIOSurfaceDrop(s);
	args->scalarOutput[0] = name;
	args->scalarOutputCount = 1;
	return name != 0 ? kIOReturnSuccess : kIOReturnNoMemory;
}

/* Requests about values start with the surface number in 8 bytes; the reply to a change is the new seed. */
static IOReturn
returnSeed(IOExternalMethodArguments * args, uint32_t seed)
{
	if (args->structureOutput != NULL && args->structureOutputSize >= sizeof(seed)) {
		memcpy(args->structureOutput, &seed, sizeof(seed));
		args->structureOutputSize = sizeof(seed);
	}
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::methodSetValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize)
{
	uint32_t sid, seed = 0;
	OSObject * obj;
	OSArray * array;
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnBadArgument;

	if (inSize < 12) {
		return kIOReturnBadArgument;
	}
	memcpy(&sid, in, sizeof(sid));
	obj = unserialize(in + 8, inSize - 8);
	array = OSDynamicCast(OSArray, obj);
	if (array == NULL) {
		OSSafeReleaseNULL(obj);
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref == NULL) {
		ret = kIOReturnNotFound;
	} else if (ref->surface->values != NULL) {
		MiniIOSurface * s = ref->surface;
		OSString * key = OSDynamicCast(OSString, array->getObject(1));
		OSDictionary * all = OSDynamicCast(OSDictionary, array->getObject(0));

		if (array->getCount() == 2 && key != NULL) {
			s->values->setObject(key, array->getObject(0));         /* [value, key] */
			ret = kIOReturnSuccess;
		} else if (array->getCount() == 1 && all != NULL) {
			s->values->merge(all);                                   /* [all the values] */
			ret = kIOReturnSuccess;
		}
		if (ret == kIOReturnSuccess) {
			s->updateValuesState();
			seed = s->shared()->valuesSeed;
		}
	}
	IOLockUnlock(gSurfaceLock);
	obj->release();
	return ret == kIOReturnSuccess ? returnSeed(args, seed) : ret;
}

IOReturn
IOSurfaceRootUserClient::methodRemoveValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize)
{
	uint32_t sid, seed = 0;
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnNotFound;

	if (inSize < 9 || in[inSize - 1] != 0) {
		return kIOReturnBadArgument;
	}
	memcpy(&sid, in, sizeof(sid));
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL && ref->surface->values != NULL) {
		MiniIOSurface * s = ref->surface;
		const char * key = (const char *) in + 8;

		if (key[0] == 0) {
			s->values->flushCollection();
		} else {
			s->values->removeObject(key);
		}
		s->updateValuesState();
		seed = s->shared()->valuesSeed;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gSurfaceLock);
	return ret == kIOReturnSuccess ? returnSeed(args, seed) : ret;
}

/* The reply: 4 bytes unused, the seed, then the serialized value (a dictionary when all were asked for). */
IOReturn
IOSurfaceRootUserClient::methodCopyValue(IOExternalMethodArguments * args, const uint8_t * in, size_t inSize)
{
	uint32_t sid, header[2] = { 0, 0 };
	struct minisurf_client_ref * ref;
	OSSerialize * s = NULL;
	IOReturn ret = kIOReturnNotFound;
	uint32_t length = 0, total;

	if (inSize < 9 || in[inSize - 1] != 0) {
		return kIOReturnBadArgument;
	}
	memcpy(&sid, in, sizeof(sid));
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL && ref->surface->values != NULL) {
		const char * key = (const char *) in + 8;
		OSObject * value = key[0] ? ref->surface->values->getObject(key) : ref->surface->values;

		header[1] = ref->surface->shared()->valuesSeed;
		ret = kIOReturnSuccess;
		if (value != NULL && (key[0] != 0 || ref->surface->values->getCount() != 0)) {
			s = OSSerialize::binaryWithCapacity(4096);
			if (s == NULL || !value->serialize(s)) {
				ret = kIOReturnNoMemory;
			} else {
				length = s->getLength();
			}
		}
	}
	IOLockUnlock(gSurfaceLock);

	total = (uint32_t) sizeof(header) + length;
	if (ret == kIOReturnSuccess) {
		if (args->structureOutputDescriptor != NULL) {
			IOMemoryDescriptor * out = args->structureOutputDescriptor;

			if (out->getLength() < total) {
				ret = kIOReturnNoSpace;
			} else if ((ret = out->prepare()) == kIOReturnSuccess) {
				out->writeBytes(0, header, sizeof(header));
				if (length != 0) {
					out->writeBytes(sizeof(header), s->text(), length);
				}
				out->complete();
				args->structureOutputDescriptorSize = total;
			}
		} else if (args->structureOutput == NULL || args->structureOutputSize < total) {
			ret = kIOReturnNoSpace;
		} else {
			memcpy(args->structureOutput, header, sizeof(header));
			if (length != 0) {
				memcpy((uint8_t *) args->structureOutput + sizeof(header), s->text(), length);
			}
			args->structureOutputSize = total;
		}
	}
	OSSafeReleaseNULL(s);
	return ret;
}

/*
 * Colour space, transfer function and the like: a fixed structure kept in the shared page.
 * The request: the structure, its size, a mask of the fields that changed, the surface number.
 */
IOReturn
IOSurfaceRootUserClient::methodSetBulkAttachments(const uint8_t * in, size_t inSize)
{
	static const struct {
		uint8_t from, to;
	} fields[15] = {
		{ 0x00, 0x20 }, { 0x20, 0x30 }, { 0x30, 0x38 }, { 0x38, 0x39 }, { 0x39, 0x3a }, { 0x3a, 0x3b }, { 0x3b, 0x3c },
		{ 0x3c, 0x3d }, { 0x3d, 0x3e }, { 0x3e, 0x3f }, { 0x3f, 0x40 }, { 0x40, 0x58 }, { 0x58, 0x5c }, { 0x5c, 0x64 },
		{ 0x64, 0x65 },
	};
	uint64_t size, mask;
	uint32_t sid;
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnNotFound;

	if (inSize < 0x88) {
		return kIOReturnBadArgument;
	}
	memcpy(&size, in + 0x70, sizeof(size));
	memcpy(&mask, in + 0x78, sizeof(mask));
	memcpy(&sid, in + 0x80, sizeof(sid));
	if (size > 0x70) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL) {
		uint8_t * attachments = ref->surface->shared()->attachments;

		for (unsigned i = 0; i < 15; i++) {
			if ((mask & (1ULL << i)) && fields[i].to <= size) {
				memcpy(attachments + fields[i].from, in + fields[i].from, fields[i].to - fields[i].from);
			}
		}
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gSurfaceLock);
	return ret;
}

IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint8_t * in = (const uint8_t *) args->structureInput;
	size_t inSize = args->structureInputSize;
	IOMemoryMap * inMap = NULL;
	struct minisurf_client_ref * ref;
	IOReturn ret;

	switch (selector) {
	case kSurfaceMethodInit:
		return methodInit(args);
	case kSurfaceMethodCreate:
		return methodCreate(args);
	case kSurfaceMethodLookup:
		return methodLookup(args);
	case kSurfaceMethodLookupFromPort:
		return methodLookupFromPort(args);
	case kSurfaceMethodCreatePort:
		return methodCreatePort(args);

	case kSurfaceMethodSetValue:
	case kSurfaceMethodCopyValue:
	case kSurfaceMethodRemoveValue:
	case kSurfaceMethodSetBulkAttachments:
		if (args->structureInputDescriptor != NULL) {
			inMap = args->structureInputDescriptor->map(kIOMapReadOnly);
			if (inMap == NULL) {
				return kIOReturnNoMemory;
			}
			in = (const uint8_t *) inMap->getVirtualAddress();
			inSize = inMap->getLength();
		}
		if (in == NULL) {
			ret = kIOReturnBadArgument;
		} else if (selector == kSurfaceMethodSetValue) {
			ret = methodSetValue(args, in, inSize);
		} else if (selector == kSurfaceMethodCopyValue) {
			ret = methodCopyValue(args, in, inSize);
		} else if (selector == kSurfaceMethodRemoveValue) {
			ret = methodRemoveValue(args, in, inSize);
		} else {
			ret = methodSetBulkAttachments(in, inSize);
		}
		OSSafeReleaseNULL(inMap);
		return ret;

	case kSurfaceMethodSetPurgeable:                /* in: surface, new state; out: old state */
		if (args->scalarInputCount < 2) {
			return kIOReturnBadArgument;
		}
		ret = kIOReturnNotFound;
		IOLockLock(gSurfaceLock);
		ref = findRef((uint32_t) args->scalarInput[0], false);
		if (ref != NULL) {
			if (args->scalarOutputCount >= 1) {
				args->scalarOutput[0] = ref->surface->purgeable;
				args->scalarOutputCount = 1;
			}
			if (args->scalarInput[1] != 3) {        /* 3 = keep current */
				ref->surface->purgeable = (uint32_t) args->scalarInput[1];
			}
			ret = kIOReturnSuccess;
		}
		IOLockUnlock(gSurfaceLock);
		return ret;

	case kSurfaceMethodBindAccel:                   /* in: surface, accelerator, sub-id */
		if (args->scalarInputCount < 3) {
			return kIOReturnBadArgument;
		}
		IOLockLock(gSurfaceLock);
		ref = findRef((uint32_t) args->scalarInput[0], false);
		if (ref != NULL) {
			ref->surface->shared()->accelID = (uint16_t) args->scalarInput[1];
			ref->surface->shared()->accelSubID = (uint16_t) args->scalarInput[2];
		}
		IOLockUnlock(gSurfaceLock);
		return ref ? kIOReturnSuccess : kIOReturnNotFound;

	case kSurfaceMethodSetYCbCrMatrix:              /* in: surface, matrix */
		if (args->scalarInputCount < 2) {
			break;
		}
		IOLockLock(gSurfaceLock);
		ref = findRef((uint32_t) args->scalarInput[0], false);
		if (ref != NULL) {
			ref->surface->shared()->ycbcrMatrix = (uint32_t) args->scalarInput[1];
		}
		IOLockUnlock(gSurfaceLock);
		return ref ? kIOReturnSuccess : kIOReturnNotFound;

	case kSurfaceMethodSetOwnership:
	case kSurfaceMethodSetOwnershipIdentity:
	case kSurfaceMethodSetTimestamp:
	case kSurfaceMethodSetIndexedTimestamp:
	case kSurfaceMethodDetachModeCode:
	case kSurfaceMethodLog:
		/* accounting and diagnostics: nothing depends on them here */
		if (args->scalarOutputCount != 0) {
			bzero(args->scalarOutput, args->scalarOutputCount * sizeof(args->scalarOutput[0]));
		}
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gSurfaceLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gSurfaceLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOSurface: selector %u not implemented (scalars %u in, %u out; structure %u in, %u out)\n", selector,
		    args->scalarInputCount, args->scalarOutputCount, args->structureInputSize, args->structureOutputSize);
	}
	return kIOReturnUnsupported;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOExternalTrap *
IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService ** targetP, UInt32 index)
{
	if (index >= kSurfaceTrapCount) {
		return NULL;
	}
	*targetP = this;
	return &fTraps[index];
}

IOReturn
IOSurfaceRootUserClient::useCountCommon(uint32_t sid, uint32_t category, int delta)
{
	struct minisurf_client_ref * ref;
	IOReturn ret = kIOReturnNotFound;

	if (category > 1) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL) {
		MiniIOSurface * s = ref->surface;

		if (delta > 0 || ref->use[category] != 0) {
			ref->use[category] += delta;
			s->useCount[category] += delta;
			s->shared()->useCount = (uint64_t) s->useCount[1] << 32 | s->useCount[0];
		}
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gSurfaceLock);
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapIncrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return useCountCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, 1);
}

IOReturn
IOSurfaceRootUserClient::trapDecrementUseCount(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return useCountCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, -1);
}

/*
 * Lock and unlock do not exclude anybody: the memory is shared and always mapped.
 * What matters is the seed, which tells readers that the pixels may have changed.
 */
IOReturn
IOSurfaceRootUserClient::lockCommon(uint32_t sid, uint32_t options, user_addr_t seedOut)
{
	struct minisurf_client_ref * ref;
	uint32_t seed = 0;

	IOLockLock(gSurfaceLock);
	ref = findRef(sid, false);
	if (ref != NULL) {
		struct minisurf_shared * sh = ref->surface->shared();

		if (!(options & kSurfaceLockReadOnly)) {
			sh->seed++;
		}
		seed = sh->seed;
	}
	IOLockUnlock(gSurfaceLock);
	if (ref == NULL) {
		return kIOReturnNotFound;
	}
	if (seedOut != 0) {
		copyout(&seed, seedOut, sizeof(seed));
	}
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapLock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return lockCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, (user_addr_t) (uintptr_t) p3);
}

IOReturn
IOSurfaceRootUserClient::trapUnlock(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	return lockCommon((uint32_t) (uintptr_t) p1, (uint32_t) (uintptr_t) p2, (user_addr_t) (uintptr_t) p3);
}

/*
 * A client drops a surface in two steps: "detach" while it still holds its own lock, and
 * "release" after it. A lookup of the same surface in between must make a fresh view.
 */
IOReturn
IOSurfaceRootUserClient::trapDetach(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	struct minisurf_client_ref * ref;

	IOLockLock(gSurfaceLock);
	ref = findRef((uint32_t) (uintptr_t) p1, false);
	if (ref != NULL) {
		ref->detached = true;
	}
	IOLockUnlock(gSurfaceLock);
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapRelease(void * p1, void * p2, void * p3, void * p4, void * p5, void * p6)
{
	struct minisurf_client_ref * ref;
	uint32_t sid = (uint32_t) (uintptr_t) p1;

	IOLockLock(gSurfaceLock);
	ref = findRef(sid, true);
	if (ref == NULL) {
		ref = findRef(sid, false);
	}
	if (ref != NULL) {
		dropRef(ref);
	}
	IOLockUnlock(gSurfaceLock);
	return ref ? kIOReturnSuccess : kIOReturnNotFound;
}
