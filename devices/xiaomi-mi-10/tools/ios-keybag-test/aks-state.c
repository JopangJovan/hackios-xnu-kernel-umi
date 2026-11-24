#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* Query only public lock-state flags. Never read or print a secret or keybag. */
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    sleep(20);
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceMatching("AppleKeyStore"));
    io_connect_t connection = IO_OBJECT_NULL;
    kern_return_t result = service ? IOServiceOpen(service, mach_task_self(), 0,
        &connection) : kIOReturnNotFound;
    printf("UMI passcode: AppleKeyStore open 0x%x\n", result);
    if (service) IOObjectRelease(service);
    if (result) return 1;
    for (unsigned sample = 0; sample < 18; ++sample) {
        uint64_t handle = 0, state = 0;
        uint32_t count = 1;
        result = IOConnectCallScalarMethod(connection, 7, &handle, 1, &state, &count);
        if (!result && count == 1)
            printf("UMI passcode: sample %u lock flags 0x%llx\n", sample,
                   (unsigned long long)state);
        else printf("UMI passcode: state query 0x%x count %u\n", result, count);
        sleep(15);
    }
    IOServiceClose(connection);
    return 0;
}
