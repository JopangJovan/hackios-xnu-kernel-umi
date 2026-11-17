#include <libkern/OSKextLib.h>
#include <kern/thread.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void umi_usb_metaclasses() __asm__("__GLOBAL__sub_I_UmiDwc3Xdci.cpp");
extern "C" bool umi_dwc3_bring_up(uint32_t options);
extern "C" void umi_dwc3_publish(void);

extern "C" void loader_log_line(const char *);
static void umi_usb_log(const char *fmt, unsigned val) {
    char line[160];
    snprintf(line, sizeof(line), fmt, val);
    loader_log_line(line);
}
static unsigned umi_usb_mode;
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);

static bool
umi_usb_wait_for_handoff()
{
    uint64_t pa=0;
    if (!PE_parse_boot_argn("umixdci_owner_pa", &pa, sizeof(pa)) || !pa || (pa & 3)) {
        loader_log_line("UMI native USB: missing handoff mailbox\n");
        return false;
    }
    constexpr vm_offset_t pageMask = 0x3fff;
    const vm_offset_t base = ml_io_map_unmappable(pa & ~pageMask, pageMask+1, VM_WIMG_WCOMB);
    if (!base) return false;
    auto *ownership = reinterpret_cast<volatile const uint32_t *>(base + (pa & pageMask));
    loader_log_line("UMI native USB: waiting for firmware to release USB\n");
    for (unsigned i=0; i<900; ++i) {
        if (*ownership == 0x58444349u) {
            __asm__ volatile("dmb oshld" ::: "memory");
            loader_log_line("UMI native USB: handoff mailbox ready\n");
            return true;
        }
        IOSleep(100);
    }
    return false;
}
static void
umi_native_usb_probe(void *, wait_result_t)
{
    umi_usb_log("UMI native USB: worker mode %u\n", umi_usb_mode);
    IOSleep(1500);
    const OSReturn controller = OSKextLoadKextWithIdentifier("com.apple.driver.AppleUSBXDCI");
    umi_usb_log("UMI native USB: AppleUSBXDCI load returned 0x%x\n", controller);
    if (umi_usb_mode == 1) {
        const OSReturn mux = OSKextLoadKextWithIdentifier("com.apple.driver.AppleUSBDeviceMux");
        umi_usb_log("UMI native USB: AppleUSBDeviceMux load returned 0x%x\n", mux);
    } else if (controller == kOSReturnSuccess || controller == kOSKextReturnStartStopError) {
        void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (handle) {
            umi_usb_metaclasses();
            const OSReturn registered = OSMetaClass::postModLoad(handle);
            umi_usb_log("UMI native USB: subclass metaclass registration returned 0x%x\n", registered);
            if (registered == kOSReturnSuccess &&
                (umi_usb_mode == 3 || (umi_usb_mode == 4 && umi_usb_wait_for_handoff())) && umi_dwc3_bring_up(1)) {
                umi_dwc3_publish();
            }
        } else {
            loader_log_line("UMI native USB: subclass metaclass registration rejected\n");
        }
    }
    thread_terminate(current_thread());
}

static void
umi_start_native_usb_probe()
{
    loader_log_line("UMI native USB: reached BSD attach hook\n");
    if (!PE_parse_boot_argn("umixdci", &umi_usb_mode, sizeof(umi_usb_mode)) ||
        !umi_usb_mode || umi_usb_mode > 4) {
        umi_usb_log("UMI native USB: no usable probe mode (%u)\n", umi_usb_mode);
        return;
    }
    umi_usb_log("UMI native USB: scheduling probe mode %u\n", umi_usb_mode);
    thread_t thread = nullptr;
    unsigned started = kernel_thread_start(umi_native_usb_probe, nullptr, &thread);
    umi_usb_log("UMI native USB: worker creation status %u\n", started);
    if (started == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
}
