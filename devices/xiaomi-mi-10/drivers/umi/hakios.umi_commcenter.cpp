/* iOS 16.5 IOKit compatibility for UMI's SDX55 mission session. */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSBoolean.h>

#include "../../bsd/sys/umi_commcenter.h"

extern "C" const uint8_t *hakios_image_file(const char *, const char *, uint64_t *, int *);
extern "C" void hakios_tl(const char *, ...) __printflike(1, 2);

enum : uint32_t {
	kReportLinkThroughput = 11,
	kBlockPDPTraffic = 16,
	kBasebandGetReset = 3,
	kBasebandIdentifyClient = 15,
	kBasebandSetManagerState = 22,
	kBasebandGetManagerState = 23,
	/* pdp_ip0 maps to the SDX55 Internet bearer on en1. */
	kInternetPDP = 0,
};

static_assert(sizeof(umi_pdp_throughput) == 32, "20F66 throughput wire layout");

class AppleIPAppenderUserClient final : public IOUserClient
{
	OSDeclareDefaultStructors(AppleIPAppenderUserClient);
	IOLock *gate = nullptr;
	bool closed = false;
	bool txHeld = false;
	bool ownsInternetPDP = false;

	void closeGate();

public:
	bool initWithTask(task_t task, void *securityID, UInt32 type, OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

class AppleIPAppender final : public IOService
{
	OSDeclareDefaultStructors(AppleIPAppender);

public:
	IOReturn newUserClient(task_t task, void *securityID, UInt32 type, OSDictionary *properties,
	    LIBKERN_RETURNS_RETAINED IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

/* The stock controller first matches this class and registers for interest.
 * The SDX55 mission worker owns hardware power and reset throughout. */
class AppleBaseband final : public IOService
{
	OSDeclareDefaultStructors(AppleBaseband);

public:
	IOReturn newUserClient(task_t task, void *securityID, UInt32 type, OSDictionary *properties,
	    LIBKERN_RETURNS_RETAINED IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

class AppleBasebandUserClient final : public IOUserClient
{
	OSDeclareDefaultStructors(AppleBasebandUserClient);

public:
	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleIPAppenderUserClient, IOUserClient)
OSDefineMetaClassAndStructors(AppleIPAppender, IOService)
OSDefineMetaClassAndStructors(AppleBaseband, IOService)
OSDefineMetaClassAndStructors(AppleBasebandUserClient, IOUserClient)

enum : unsigned {
	kSDX55Online = 1u << 0,
	kAppleManagerStarted = 1u << 1,
};
static unsigned basebandState;

bool
AppleIPAppenderUserClient::initWithTask(task_t task, void *securityID, UInt32 type, OSDictionary *properties)
{
	if (!IOUserClient::initWithTask(task, securityID, type, properties)) {
		return false;
	}
	/* Stock CommCenter has no binding to UMI's WDS session yet. The
	 * entitled RAM integration probe exercises the confirmed mapping. */
	auto entitlement = IOUserClient::copyClientEntitlement(task, "com.hakios.sdx55-pdp-control");
	ownsInternetPDP = entitlement == kOSBooleanTrue;
	OSSafeReleaseNULL(entitlement);
	return true;
}

bool
AppleIPAppenderUserClient::start(IOService *provider)
{
	gate = IOLockAlloc();
	return gate && IOUserClient::start(provider);
}

void
AppleIPAppenderUserClient::closeGate()
{
	IOLockLock(gate);
	closed = true;
	if (txHeld) {
		umi_sdx55_net_hold_tx(false);
		txHeld = false;
	}
	IOLockUnlock(gate);
}

IOReturn
AppleIPAppenderUserClient::clientClose()
{
	closeGate();
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
AppleIPAppenderUserClient::free()
{
	if (gate) {
		closeGate();
		IOLockFree(gate);
	}
	IOUserClient::free();
}

IOReturn
AppleIPAppenderUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	if (selector != kReportLinkThroughput && selector != kBlockPDPTraffic) {
		return kIOReturnUnsupported;
	}
	if (!args || args->asyncWakePort != MACH_PORT_NULL ||
	    args->scalarOutputCount || args->structureOutputSize || args->structureOutputDescriptor) {
		return kIOReturnBadArgument;
	}
	if (selector == kReportLinkThroughput) {
		if (args->scalarInputCount || !args->structureInput || args->structureInputDescriptor ||
		    args->structureInputSize != sizeof(umi_pdp_throughput)) {
			return kIOReturnBadArgument;
		}
		umi_pdp_throughput rates;
		memcpy(&rates, args->structureInput, sizeof(rates));
		IOLockLock(gate);
		if (closed) {
			IOLockUnlock(gate);
			return kIOReturnNotOpen;
		}
		int error = umi_sdx55_net_report_throughput(&rates);
		IOLockUnlock(gate);
		static unsigned throughputLogs;
		if (__atomic_fetch_add(&throughputLogs, 1, __ATOMIC_RELAXED) < 12) {
			hakios_tl("UMI CommCenter: en1 throughput errno=%d\n", error);
		}
		return error ? kIOReturnNotReady : kIOReturnSuccess;
	}
	if (args->scalarInputCount != 2 || !args->scalarInput ||
	    args->structureInputSize || args->structureInputDescriptor || args->scalarInput[1] > 1) {
		return kIOReturnBadArgument;
	}
	if (args->scalarInput[0] != kInternetPDP) {
		static unsigned unsupportedLogs;
		if (__atomic_fetch_add(&unsupportedLogs, 1, __ATOMIC_RELAXED) < 12) {
			hakios_tl("UMI CommCenter: unsupported PDP context %llu\n", args->scalarInput[0]);
		}
		return kIOReturnUnsupported;
	}
	if (!ownsInternetPDP) {
		static unsigned unboundLogs;
		if (__atomic_fetch_add(&unboundLogs, 1, __ATOMIC_RELAXED) < 12) {
			hakios_tl("UMI CommCenter: PDP request has no SDX55 bearer binding\n");
		}
		return kIOReturnNotReady;
	}
	bool hold = args->scalarInput[1] != 0;
	IOLockLock(gate);
	bool applied = !closed;
	if (applied && txHeld != hold) {
		umi_sdx55_net_hold_tx(hold);
		txHeld = hold;
	}
	IOLockUnlock(gate);
	static unsigned pdpLogs;
	if (__atomic_fetch_add(&pdpLogs, 1, __ATOMIC_RELAXED) < 24) {
		hakios_tl("UMI CommCenter: Internet PDP TX hold=%u applied=%u\n", hold, applied);
	}
	return applied ? kIOReturnSuccess : kIOReturnNotOpen;
}

template <class Client>
static IOReturn
openClient(IOService *provider, task_t task, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler, const char *access)
{
	if (type || !handler) {
		return kIOReturnBadArgument;
	}
	*handler = nullptr;
	auto entitlement = IOUserClient::copyClientEntitlement(task, access);
	bool entitled = entitlement == kOSBooleanTrue;
	OSSafeReleaseNULL(entitlement);
	if (!entitled) {
		return kIOReturnNotPrivileged;
	}
	auto *client = OSTypeAlloc(Client);
	if (!client) {
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(task, securityID, type, properties) || !client->attach(provider)) {
		client->release();
		return kIOReturnNoResources;
	}
	if (!client->start(provider)) {
		client->detach(provider);
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}

IOReturn
AppleIPAppender::newUserClient(task_t task, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
	return openClient<AppleIPAppenderUserClient>(this, task, securityID, type, properties,
	    handler, "com.apple.driver.AppleIPAppender.user-access");
}

IOReturn
AppleBaseband::newUserClient(task_t task, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
	IOReturn result = openClient<AppleBasebandUserClient>(this, task, securityID, type, properties,
	    handler, "com.apple.driver.AppleBasebandPCI.user-access");
	hakios_tl("UMI CommCenter: AppleBaseband open type=%u result=%#x\n", type, result);
	return result;
}

IOReturn
AppleBasebandUserClient::clientClose()
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

IOReturn
AppleBasebandUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	unsigned state = __atomic_load_n(&basebandState, __ATOMIC_ACQUIRE);
	if (selector == kBasebandGetReset || selector == kBasebandGetManagerState ||
	    selector == kBasebandSetManagerState) {
		if (!args || args->asyncWakePort != MACH_PORT_NULL || args->structureInputSize ||
		    args->structureInputDescriptor || args->structureOutputSize || args->structureOutputDescriptor) {
			return kIOReturnBadArgument;
		}
		if (selector == kBasebandSetManagerState) {
			if (args->scalarInputCount != 1 || !args->scalarInput || args->scalarOutputCount ||
			    args->scalarInput[0] > 1) {
				return kIOReturnBadArgument;
			}
			/* ABM's manager lifetime is independent of modem power. */
			unsigned requested = kSDX55Online | (args->scalarInput[0] ? kAppleManagerStarted : 0);
			while ((state & kSDX55Online) && !isInactive()) {
				if (__atomic_compare_exchange_n(&basebandState, &state, requested, false,
				    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
					return kIOReturnSuccess;
				}
			}
			return kIOReturnNotReady;
		}
		if (args->scalarInputCount || args->scalarOutputCount != 1 || !args->scalarOutput) {
			return kIOReturnBadArgument;
		}
		if (!(state & kSDX55Online) || isInactive()) {
			return kIOReturnNotReady;
		}
		args->scalarOutput[0] = selector == kBasebandGetManagerState && (state & kAppleManagerStarted);
		static unsigned statusLogs;
		if (__atomic_fetch_add(&statusLogs, 1, __ATOMIC_RELAXED) < 12) {
			hakios_tl("UMI CommCenter: AppleBaseband status selector=%u value=%llu\n",
			    selector, args->scalarOutput[0]);
		}
		return kIOReturnSuccess;
	}
	if (selector != kBasebandIdentifyClient) {
		static unsigned unsupportedLogs;
		if (__atomic_fetch_add(&unsupportedLogs, 1, __ATOMIC_RELAXED) < 24 && args) {
			hakios_tl("UMI CommCenter: AppleBaseband selector=%u in=%u/%u out=%u/%u unsupported\n",
			    selector, args->scalarInputCount, args->structureInputSize,
			    args->scalarOutputCount, args->structureOutputSize);
		}
		return kIOReturnUnsupported;
	}
	/* TelephonyBaseband formats its client label into a fixed 64-byte buffer. */
	constexpr size_t labelSize = 64;
	if (!args || args->asyncWakePort != MACH_PORT_NULL || args->scalarInputCount ||
	    args->scalarOutputCount || args->structureOutputSize || args->structureOutputDescriptor ||
	    !args->structureInput || args->structureInputDescriptor || args->structureInputSize != labelSize) {
		return kIOReturnBadArgument;
	}
	const char *label = static_cast<const char *>(args->structureInput);
	if (!label[0] || !memchr(label, '\0', labelSize)) {
		return kIOReturnBadArgument;
	}
	if (!setProperty("ClientName", label)) {
		return kIOReturnNoMemory;
	}
	hakios_tl("UMI CommCenter: AppleBaseband client identified\n");
	return kIOReturnSuccess;
}

static IOService *appender;
static IOService *baseband;

static IOService *
attachService(IOService *service, IOService *provider)
{
	if (!service) {
		return nullptr;
	}
	if (service->init() && service->attach(provider)) {
		if (service->start(provider)) {
			return service;
		}
		service->detach(provider);
	}
	service->release();
	return nullptr;
}

void
umi_commcenter_start()
{
	uint64_t length = 0;
	const uint8_t *enabled = hakios_image_file("etc", "umi-commcenter", &length, nullptr);
	if (!enabled || length != 2 || enabled[0] != '1' || enabled[1] != '\n') {
		return;
	}
	IOService *provider = IOService::getServiceRoot();
	if (!provider) {
		return;
	}
	appender = attachService(OSTypeAlloc(AppleIPAppender), provider);
	baseband = attachService(OSTypeAlloc(AppleBaseband), provider);
	/* TelephonyBaseband treats bootstage >= 2 as an awake modem. DMS has
	 * already confirmed the SDX55 mission session before publication. */
	if (!appender || !baseband || !baseband->setProperty("bootstage", 2, 32) ||
	    !baseband->setProperty("UMIExternallyBooted", kOSBooleanTrue)) {
		umi_commcenter_stop();
		hakios_tl("UMI CommCenter: could not publish compatibility services\n");
		return;
	}
	appender->registerService(kIOServiceSynchronous);
	__atomic_store_n(&basebandState, kSDX55Online, __ATOMIC_RELEASE);
	baseband->registerService(kIOServiceSynchronous);
	hakios_tl("UMI CommCenter: IPAppender and live SDX55 baseband published\n");
}

void
umi_commcenter_stop()
{
	__atomic_store_n(&basebandState, 0u, __ATOMIC_RELEASE);
	if (baseband) {
		baseband->removeProperty("UMIExternallyBooted");
		baseband->setProperty("bootstage", uint64_t{0}, 32);
		baseband->terminate(kIOServiceRequired);
		OSSafeReleaseNULL(baseband);
	}
	if (appender) {
		appender->terminate(kIOServiceRequired);
		OSSafeReleaseNULL(appender);
	}
}
