/* Independent mobile client of the stock registry, Gestalt and CoreTelephony. */
#import <CoreFoundation/CoreFoundation.h>
#import <CoreTelephony/CTTelephonyNetworkInfo.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>

extern CFTypeRef MGCopyAnswer(CFStringRef question, CFDictionaryRef options);

static int
boolean_answer(CFStringRef question)
{
    CFTypeRef answer = MGCopyAnswer(question, NULL);
    if (!answer) return -1;
    int result = CFGetTypeID(answer) == CFBooleanGetTypeID() ? (int)CFBooleanGetValue(answer) : -1;
    CFRelease(answer);
    return result;
}

/* Apple DT nubs publish untyped property bytes as OSData. */
static BOOL
property_equals(io_registry_entry_t node, CFStringRef key, const void *bytes, size_t size)
{
    CFTypeRef value = IORegistryEntryCreateCFProperty(node, key, kCFAllocatorDefault, 0);
    if (!value) return NO;
    BOOL result = CFGetTypeID(value) == CFDataGetTypeID() &&
        CFDataGetLength(value) == (CFIndex)size && memcmp(CFDataGetBytePtr(value), bytes, size) == 0;
    CFRelease(value);
    return result;
}

int
main(void)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        io_registry_entry_t node = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/arm-io/baseband");
        const char compatible[] = "qcom,ext-sdx55m";
        const uint32_t vendor = 0x17cb, device = 0x0306;
        BOOL matches = node && property_equals(node, CFSTR("compatible"), compatible, sizeof(compatible)) &&
            property_equals(node, CFSTR("vendor-id"), &vendor, sizeof(vendor)) &&
            property_equals(node, CFSTR("device-id"), &device, sizeof(device));
        if (node) IOObjectRelease(node);
        printf("UMI baseband: node=%d hardware_match=%d\n", node != 0, (int)matches);

        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            @autoreleasepool {
                int baseband = boolean_answer(CFSTR("HasBaseband"));
                int cellular = boolean_answer(CFSTR("cellular-data"));
                printf("UMI baseband: HasBaseband=%d cellular-data=%d\n", baseband, cellular);
                CTTelephonyNetworkInfo *info = [[CTTelephonyNetworkInfo alloc] init];
                NSDictionary *services = info.serviceCurrentRadioAccessTechnology;
                printf("UMI baseband: radio_services=%ld\n", services ? (long)services.count : -1L);
                dispatch_semaphore_signal(done);
            }
        });
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC)))
            puts("UMI baseband: queries=timeout");
    }
    return 0;
}
