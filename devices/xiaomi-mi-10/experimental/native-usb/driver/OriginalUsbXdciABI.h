// iPadOS 16.5 (20F66), AppleUSBXDCI and AppleT8011USBXDCI layout.
#pragma once
#include <IOKit/IOService.h>
constexpr unsigned kXdciRegisterMap = 0x200;
constexpr unsigned kXdciRegisters = 0x208;
constexpr unsigned kXdciParameters = 0x23e8;
#define XDCI_KEEP_CONFIGURATIONS opaque179
#define XDCI_TRANSPORT_ACTIVE opaque254
#define XDCI_POWER_ON opaque255
#define XDCI_POWER_OFF opaque256
#define XDCI_PARAMETERS opaque257
#define XDCI_CONTROLLER_RESET opaque261
#define XDCI_PERIODIC_ON opaque262
#define XDCI_PERIODIC_OFF opaque263
class AppleUSBXDCI : public IOService {
 OSDeclareAbstractStructors(AppleUSBXDCI)
 unsigned char originalPrivate[0x2410-sizeof(IOService)];
public:
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *) override;
 virtual void opaque168();
 virtual void opaque169();
 virtual void opaque170();
 virtual void opaque171();
 virtual void opaque172();
 virtual void opaque173();
 virtual void opaque174();
 virtual void opaque175();
 virtual void opaque176();
 virtual void opaque177();
 virtual void opaque178();
 virtual bool opaque179()=0;
 virtual void opaque180();
 virtual void opaque181();
 virtual void opaque182();
 virtual void opaque183();
 virtual void opaque184();
 virtual void opaque185();
 virtual void opaque186();
 virtual void opaque187();
 virtual void opaque188();
 virtual void opaque189();
 virtual void opaque190();
 virtual void opaque191();
 virtual void opaque192();
 virtual void opaque193();
 virtual void opaque194();
 virtual void opaque195();
 virtual void opaque196();
 virtual void opaque197();
 virtual void opaque198();
 virtual void opaque199();
 virtual void opaque200();
 virtual void opaque201();
 virtual void opaque202();
 virtual void opaque203();
 virtual void opaque204();
 virtual void opaque205();
 virtual void opaque206();
 virtual void opaque207();
 virtual void opaque208();
 virtual void opaque209();
 virtual void opaque210();
 virtual void opaque211();
 virtual void opaque212();
 virtual void opaque213();
 virtual void opaque214();
 virtual void opaque215();
 virtual void opaque216();
 virtual void opaque217();
 virtual void opaque218();
 virtual void opaque219();
 virtual void opaque220();
 virtual void opaque221();
 virtual void opaque222();
 virtual void opaque223();
 virtual void opaque224();
 virtual void opaque225();
 virtual void opaque226();
 virtual void opaque227();
 virtual void opaque228();
 virtual void opaque229();
 virtual void opaque230();
 virtual void opaque231();
 virtual void opaque232();
 virtual void opaque233();
 virtual void opaque234();
 virtual void opaque235();
 virtual void opaque236();
 virtual void opaque237();
 virtual void opaque238();
 virtual void opaque239();
 virtual void opaque240();
 virtual void opaque241();
 virtual void opaque242();
 virtual void opaque243();
 virtual void opaque244();
 virtual void opaque245();
 virtual void opaque246();
 virtual void opaque247();
 virtual void opaque248();
 virtual void opaque249();
 virtual void opaque250();
 virtual void opaque251();
 virtual void opaque252();
 virtual void opaque253();
 virtual bool opaque254()=0;
 virtual IOReturn opaque255()=0;
 virtual IOReturn opaque256()=0;
 virtual void opaque257()=0;
 virtual void opaque258();
 virtual void opaque259();
 virtual void opaque260();
 virtual IOReturn opaque261();
 virtual void opaque262()=0;
 virtual void opaque263()=0;
 virtual void opaque264();
};
static_assert(sizeof(AppleUSBXDCI) == 0x2410, "AppleUSBXDCI ABI");
