/* Attach iOS 20F66 ABM to a modem already booted by the UMI SDX55 driver. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

void umi_commcenter_install_state_bridge(void);

static io_service_t copy_sdx55_service(void)
{
    io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("AppleBaseband"));
    if (!service) return MACH_PORT_NULL;
    CFTypeRef external = IORegistryEntryCreateCFProperty(service, CFSTR("UMIExternallyBooted"), NULL, 0);
    bool sdx55 = external == kCFBooleanTrue;
    if (external) CFRelease(external);
    if (sdx55) return service;
    IOObjectRelease(service);
    return MACH_PORT_NULL;
}

static bool sdx55_online(void)
{
    io_service_t service = copy_sdx55_service();
    if (!service) return false;
    io_connect_t connection = MACH_PORT_NULL;
    kern_return_t result = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (result != KERN_SUCCESS) return false;

    uint64_t reset = 1;
    uint32_t count = 1;
    result = IOConnectCallScalarMethod(connection, 3, NULL, 0, &reset, &count);
    IOServiceClose(connection);
    return result == KERN_SUCCESS && count == 1 && reset == 0;
}

/* Both 20F66 methods return C++ bool and take a controller and a reference
 * to the error string. ABM's ordinary completion owns the boot state. */
static bool attach_running_modem(__unused void *controller, __unused void *error)
{
    bool ready = sdx55_online();
    fprintf(stderr, "UMI ABM: SDX55 attachment check ready=%u\n", ready);
    return ready;
}

static bool omit_pci_event_listener(void *abm)
{
    uint32_t *change_state = dlsym(abm, "_ZN10BootModule16changeState_syncENS_5StateE");
    /* 20F66 passes the return slot in x8 and already handles an empty
     * shared_ptr. With no Apple PCI control service, the factory crashes
     * in the destructor before it can return the empty result. */
    const unsigned create = 0x1dc / sizeof(uint32_t);
    if (!change_state || change_state[create - 2] != 0x910103e8 ||
        change_state[create] != 0x951c9097 || change_state[create + 1] != 0xa94427e8) {
        return false;
    }
    uint32_t *call = &change_state[create];
    vm_address_t page = (vm_address_t)call & ~(vm_page_size - 1);
    if (vm_protect(mach_task_self(), page, vm_page_size, false,
        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
        return false;
    }
    *call = 0xa9007d1f; /* stp xzr, xzr, [x8]: no Apple PCI listener */
    sys_icache_invalidate(call, sizeof(*call));
    if (vm_protect(mach_task_self(), page, vm_page_size, false,
        VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) {
        _exit(1);
    }
    return true;
}

__attribute__((constructor)) static void attach_sdx55(void)
{
    if (!sdx55_online()) {
        fprintf(stderr, "UMI ABM: externally booted SDX55 service unavailable\n");
        return;
    }

    /* AppleBasebandManager opens this implementation dynamically after main. */
    void *abm = dlopen("/usr/lib/libBasebandManager.dylib", RTLD_LOCAL | RTLD_LAZY);
    if (!abm) {
        fprintf(stderr, "UMI ABM: unable to load Apple boot module (%s)\n", dlerror());
        return;
    }
    void **table = dlsym(abm, "_ZTV20BootControllerLegacy");
    void *prepare = dlsym(abm, "_ZN20BootControllerLegacy7prepareERNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE");
    void *boot = dlsym(abm, "_ZN20BootControllerLegacy9bootModemERNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE");
    /* Two header words precede the 20F66 vtable address point. BootModule
     * then invokes the methods at offsets 0x30 and 0x38. */
    enum { prepare_slot = 8, boot_slot = 9 };
    if (!table || !prepare || !boot || table[prepare_slot] != prepare || table[boot_slot] != boot) {
        fprintf(stderr, "UMI ABM: 20F66 boot controller did not match\n");
        dlclose(abm);
        return;
    }

    vm_address_t page = (vm_address_t)&table[prepare_slot] & ~(vm_page_size - 1);
    vm_address_t end = ((vm_address_t)&table[boot_slot + 1] + vm_page_size - 1) & ~(vm_page_size - 1);
    kern_return_t result = vm_protect(mach_task_self(), page, end - page, false,
        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "UMI ABM: unable to copy boot controller table (%d)\n", result);
        dlclose(abm);
        return;
    }
    bool listener_compatible = omit_pci_event_listener(abm);
    if (listener_compatible) {
        table[prepare_slot] = attach_running_modem;
        table[boot_slot] = attach_running_modem;
    }
    result = vm_protect(mach_task_self(), page, end - page, false, VM_PROT_READ);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "UMI ABM: unable to restore boot controller table protection (%d)\n", result);
        _exit(1);
    }
    if (!listener_compatible) {
        fprintf(stderr, "UMI ABM: 20F66 PCI listener failure path unavailable\n");
        dlclose(abm);
        return;
    }
    /* Keep the patched image resident until AppleBasebandManager opens it. */
    umi_commcenter_install_state_bridge();
    fprintf(stderr, "UMI ABM: selected existing-modem attach for SDX55\n");
}
