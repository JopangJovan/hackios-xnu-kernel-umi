/*
 * mg_test: what MobileGestalt says about this device (the answers iOS builds its idea of the
 * hardware from).   usage: mg_test [KEY...]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

extern CFTypeRef MGCopyAnswer(CFStringRef key);

static const char *const keys[] = {
	"DeviceClass", "DeviceClassNumber", "DeviceName", "ProductType", "HWModelStr", "ModelNumber", "ProductName",
	"marketing-name", "UserAssignedDeviceName", "ComputerName", "SerialNumber", "UniqueDeviceID", "HasBaseband",
	"telephony", "cellular-data", "wifi", "bluetooth", "DeviceSupportsTelephonyOverUSB", "main-screen-scale",
	"main-screen-width", "main-screen-height", "main-screen-class", "ArtworkDeviceSubType", "DeviceColor", "CPUArchitecture",
	"BuildVersion", "ProductVersion", "RegionInfo", "front-facing-camera", "rear-facing-camera", "still-camera",
};

int
main(int argc, char **argv)
{
	int count = argc > 1 ? argc - 1 : (int)(sizeof(keys) / sizeof(keys[0]));

	for (int i = 0; i < count; i++) {
		const char *name = argc > 1 ? argv[i + 1] : keys[i];
		CFStringRef key = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
		CFTypeRef answer = MGCopyAnswer(key);
		CFStringRef text = answer ? CFCopyDescription(answer) : NULL;
		char line[256] = "(none)";

		if (text) {
			CFStringGetCString(text, line, sizeof(line), kCFStringEncodingUTF8);
			CFRelease(text);
		}
		printf("mg_test: %s = %s\n", name, line);
		if (answer) {
			CFRelease(answer);
		}
		CFRelease(key);
	}
	return 0;
}
