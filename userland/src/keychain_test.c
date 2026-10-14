/*
 * keychain_test: write a keychain item (a generic password), read it back, delete it, and print what
 * Security.framework answers each time: 0 is success, -25300 "not found", -25291 "not available"
 * (what the keychain answered while the kernel's AppleKeyStore stand-in did not wrap keys),
 * -34018 "missing entitlement" (built unsigned: it is signed with keychain_test.entitlements).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <stdio.h>
#include <string.h>

static CFMutableDictionaryRef
query(void)
{
	CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(q, kSecAttrService, CFSTR("hackios-keychain-test"));
	CFDictionarySetValue(q, kSecAttrAccount, CFSTR("test"));
	return q;
}

int
main(void)
{
	static const char secret[] = "hello from the keychain";
	CFMutableDictionaryRef q = query();
	CFDataRef data = CFDataCreate(NULL, (const UInt8 *)secret, sizeof(secret) - 1), back = NULL;
	OSStatus status;

	setvbuf(stdout, NULL, _IOLBF, 0);
	status = SecItemDelete(q);
	printf("keychain_test: delete before: %d\n", (int)status);
	CFDictionarySetValue(q, kSecValueData, data);
	CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
	status = SecItemAdd(q, NULL);
	printf("keychain_test: add: %d\n", (int)status);
	CFRelease(q);
	q = query();
	CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
	status = SecItemCopyMatching(q, (CFTypeRef *)&back);
	printf("keychain_test: read back: %d, %s\n", (int)status, back != NULL && CFGetTypeID(back) == CFDataGetTypeID() &&
	    CFEqual(back, data) ? "THE SAME BYTES" : back != NULL ? "other bytes" : "nothing");
	CFRelease(q);
	q = query();
	status = SecItemDelete(q);
	printf("keychain_test: delete: %d\n", (int)status);
	return 0;
}
