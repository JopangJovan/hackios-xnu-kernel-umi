/*
 * GenericARMPE: a minimal platform expert for non-Apple ARMv8 boards.
 *
 * On Apple hardware the platform expert comes from a closed kext
 * (AppleARMPlatform). Without one, IOKit falls through to IOPanicPlatform
 * ("Unable to find driver for this platform"). This class is the least that
 * lets IOKit start on a board described by a loader-built device tree:
 * it publishes the device-tree nodes as nubs and names the machine.
 */
extern "C" {
#include <pexpert/pexpert.h>
#include <kern/cpu_number.h>
}
#include <machine/machine_routines.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCPU.h>
#include <kern/clock.h>

#include "MiniDrivers.h"

extern "C" kern_return_t processor_start(processor_t processor);

/*
 * GenericARMCPU: registers a device-tree CPU node with the kernel. The closed
 * AppleARMCPU driver does this on Apple hardware; without it the kernel waits
 * forever for the CPU count (ml_wait_max_cpus) and never calls
 * cpu_machine_init() on the boot processor.
 *
 * Only the boot CPU is brought up for now. Secondary CPUs need a start
 * method (PSCI CPU_ON) and inter-processor interrupts through the GIC.
 */
class GenericARMCPU : public IOCPU
{
	OSDeclareDefaultStructors(GenericARMCPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual void initCPU(bool boot) APPLE_KEXT_OVERRIDE;
	virtual void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
	virtual kern_return_t startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr) APPLE_KEXT_OVERRIDE;
	virtual void haltCPU(void) APPLE_KEXT_OVERRIDE;
	virtual const OSSymbol * getCPUName(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(GenericARMCPU, IOCPU);

bool
GenericARMCPU::start(IOService * provider)
{
	ml_processor_info_t info;
	perfmon_interrupt_handler_func pmi_handler = NULL;
	uint32_t phys_id = 0;

	if (!IOCPU::start(provider)) {
		return false;
	}

	OSData * reg = OSDynamicCast(OSData, provider->getProperty("reg"));
	if (reg != NULL && reg->getLength() >= sizeof(uint32_t)) {
		phys_id = *(const uint32_t *)reg->getBytesNoCopy();
	}

	setCPUNumber((UInt32)ml_get_boot_cpu_number());

	bzero(&info, sizeof(info));
	info.cpu_id = (cpu_id_t)this;
	info.log_id = getCPUNumber();
	info.phys_id = phys_id;
	info.cluster_type = CLUSTER_TYPE_SMP;
	if (ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi_handler) != KERN_SUCCESS) {
		IOLog("GenericARMCPU: ml_processor_register failed\n");
		return false;
	}

	/* Unblocks everything that waits for the CPU count to be known. */
	ml_set_max_cpus(1);

	setCPUState(kIOCPUStateUninitalized);
	registerService();

	/* Runs cpu_machine_init() -> initCPU(true) on this (the boot) processor. */
	processor_start(machProcessor);
	IOLog("GenericARMCPU: cpu %u (phys id 0x%x) registered\n", (unsigned)getCPUNumber(), phys_id);
	return true;
}

void
GenericARMCPU::initCPU(bool boot)
{
	/* The interrupt controller and timer were set up in arm_init (pe_init_fiq). */
	setCPUState(kIOCPUStateRunning);
}

void
GenericARMCPU::quiesceCPU(void)
{
}

kern_return_t
GenericARMCPU::startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr)
{
	return KERN_NOT_SUPPORTED;
}

void
GenericARMCPU::haltCPU(void)
{
}

const OSSymbol *
GenericARMCPU::getCPUName(void)
{
	char name[32];
	snprintf(name, sizeof(name), "Primary%u", (unsigned)getCPUNumber());
	return OSSymbol::withCString(name);
}

class GenericARMPE : public IODTPlatformExpert
{
	OSDeclareDefaultStructors(GenericARMPE);

public:
	virtual IOService * probe(IOService * provider, SInt32 * score) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual const char * deleteList(void) APPLE_KEXT_OVERRIDE;
	virtual const char * excludeList(void) APPLE_KEXT_OVERRIDE;
	virtual bool getMachineName(char * name, int maxLength) APPLE_KEXT_OVERRIDE;
	virtual long getGMTTimeOfDay(void) APPLE_KEXT_OVERRIDE;
	virtual void setGMTTimeOfDay(long secs) APPLE_KEXT_OVERRIDE;
};

#define super IODTPlatformExpert

OSDefineMetaClassAndStructors(GenericARMPE, IODTPlatformExpert);

IOService *
GenericARMPE::probe(IOService * provider, SInt32 * score)
{
	/*
	 * IODTPlatformExpert::probe() wants an IONameMatch against the root
	 * node; this expert is the only one built in, so it takes any board.
	 */
	if (!IOPlatformExpert::probe(provider, score)) {
		return NULL;
	}
	return this;
}

bool
GenericARMPE::start(IOService * provider)
{
	if (!super::start(provider)) {
		return false;
	}
	IOLog("GenericARMPE: platform \"%s\"\n", provider->getName());

	/*
	 * super::start() published the /cpus children as nubs. Attach a CPU
	 * driver to the boot processor's nub (device-tree state "running").
	 */
	OSIterator * iter = getChildIterator(gIOServicePlane);
	if (iter != NULL) {
		OSObject * obj;
		while ((obj = iter->getNextObject()) != NULL) {
			IOService * nub = OSDynamicCast(IOService, obj);
			OSData * state = nub ? OSDynamicCast(OSData, nub->getProperty("state")) : NULL;

			if (state == NULL || strncmp((const char *)state->getBytesNoCopy(), "running", state->getLength()) != 0) {
				continue;
			}
			GenericARMCPU * cpu = new GenericARMCPU;
			if (cpu != NULL && cpu->init() && cpu->attach(nub)) {
				if (!cpu->start(nub)) {
					cpu->detach(nub);
				}
			}
			if (cpu != NULL) {
				cpu->release();
			}
		}
		iter->release();
	}

	/* BSD looks the platform expert up by class (IOSecureBSDRoot). */
	registerService();

	/* Stand-ins for the closed graphics drivers that the iOS window server talks to. */
	MiniIOSurfaceStart(this);
	MiniIOMobileFramebufferStart(this);
	MiniAppleKeyStoreStart();
	return true;
}

/*
 * The time of day. There is no driver for a real-time clock here; whoever started the kernel
 * says what time it was then -- the boot-arg epoch= (seconds since 1970, put there by the
 * host scripts) or, failing that, the property boot-epoch of /chosen (the loader, from the
 * firmware's clock) -- and the kernel's own timer counts from there. Without either the
 * calendar starts in 1970, as before.
 */
static long gBootEpoch;         /* the calendar time at which the system timer read zero */
static bool gBootEpochKnown;

static void
findBootEpoch(void)
{
	uint64_t epoch = 0;

	if (gBootEpochKnown) {
		return;
	}
	gBootEpochKnown = true;
	if (!PE_parse_boot_argn("epoch", &epoch, sizeof(epoch)) || epoch == 0) {
		IORegistryEntry * chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
		OSData * data = chosen ? OSDynamicCast(OSData, chosen->getProperty("boot-epoch")) : NULL;

		if (data != NULL && data->getLength() == sizeof(epoch)) {
			memcpy(&epoch, data->getBytesNoCopy(), sizeof(epoch));
		}
		OSSafeReleaseNULL(chosen);
	}
	if (epoch != 0) {
		clock_sec_t secs;
		clock_usec_t usecs;

		clock_get_system_microtime(&secs, &usecs);
		gBootEpoch = (long) epoch - (long) secs;
	}
}

long
GenericARMPE::getGMTTimeOfDay(void)
{
	clock_sec_t secs;
	clock_usec_t usecs;

	findBootEpoch();
	if (gBootEpoch == 0) {
		return 0;
	}
	clock_get_system_microtime(&secs, &usecs);
	return gBootEpoch + (long) secs;
}

void
GenericARMPE::setGMTTimeOfDay(long secs)
{
	clock_sec_t uptime;
	clock_usec_t usecs;

	clock_get_system_microtime(&uptime, &usecs);
	gBootEpoch = secs - (long) uptime;
	gBootEpochKnown = true;
}

/* Device-tree nodes to drop entirely. */
const char *
GenericARMPE::deleteList(void)
{
	return "('packages', 'psuedo-usb', 'psuedo-hid', 'multiboot', 'rtas')";
}

/* Top-level nodes that describe boot state rather than devices: no nubs for these. */
const char *
GenericARMPE::excludeList(void)
{
	return "('chosen', 'memory', 'options', 'aliases', 'defaults', 'pram', 'cpus')";
}

/*
 * hw.machine. On Apple's devices it is the product type ("iPhone10,3"), from the device tree's
 * "model", and iOS decides from it what kind of device it is on: with anything it does not know
 * MobileGestalt settles for an iPod touch.
 */
bool
GenericARMPE::getMachineName(char * name, int maxLength)
{
	uint32_t real = 1;

	/*
	 * hw.machine is the model the loader put into the device tree, "iPhone10,3". It has to
	 * begin with "iPhone": CoreFoundation picks the device-specific variants of resources
	 * (Something~iphone.plist, Key~iphone in an Info.plist) by this string, and for a name it
	 * does not know it picks none at all -- Control Center, for one, then lays its modules out
	 * without their sizes. (boot-arg machine_name=0: the plain "arm64" this used to be.)
	 */
	PE_parse_boot_argn("machine_name", &real, sizeof(real));
	if (!real || !IODTPlatformExpert::getMachineName(name, maxLength) || name[0] == 0) {
		strlcpy(name, "arm64", maxLength);
	}
	return true;
}
