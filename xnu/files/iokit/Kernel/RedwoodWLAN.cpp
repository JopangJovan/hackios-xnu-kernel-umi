/*
 * RedwoodWLAN: the registry entry by which iOS finds the Wi-Fi interface.
 *
 * en0 is made with the BSD interface KPI (bsd/dev/redwood_net.c) and has no IOKit object behind
 * it; iOS's Wi-Fi side looks for one before anything else. wifid's device is whatever service
 * has the property IO80211InterfaceRole = "Infrastructure", and IO80211.framework binds to an
 * interface through a service whose IOInterfaceName is the interface's name. This is that
 * service and nothing more: properties. It has no user client, on purpose: the framework's
 * attempt to open one fails, it says "Falling back to IOCTL path", and from then on every
 * request is an ioctl on a socket, which bsd/dev/redwood_apple80211.c answers.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>

extern "C" void RedwoodWLANStart(const char * interface);

class RedwoodWLAN : public IOService
{
	OSDeclareDefaultStructors(RedwoodWLAN);

public:
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties,
	    IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodWLAN, IOService);

/* There is none (see the top); that somebody asked shows that the service was found. */
IOReturn
RedwoodWLAN::newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties, IOUserClient ** handler)
{
	static unsigned said;

	if (said++ < 8) {
		IOLog("RedwoodWLAN: a user client of type %u is asked for: there is none\n", (unsigned) type);
	}
	return kIOReturnUnsupported;
}

/* Once, when the interface of a radio exists. */
void
RedwoodWLANStart(const char * interface)
{
	IOService * provider = IOService::getPlatform();
	RedwoodWLAN * wlan = OSTypeAlloc(RedwoodWLAN);

	if (wlan == NULL || provider == NULL || !wlan->init() || !wlan->attach(provider)) {
		OSSafeReleaseNULL(wlan);
		IOLog("RedwoodWLAN: cannot create the service\n");
		return;
	}
	wlan->setName("RedwoodWLAN");
	wlan->setProperty("IOInterfaceName", interface);
	wlan->setProperty("IO80211InterfaceRole", "Infrastructure");
	if (!wlan->start(provider)) {
		wlan->detach(provider);
		IOLog("RedwoodWLAN: the service did not start\n");
	} else {
		wlan->registerService();
	}
	wlan->release();
}
