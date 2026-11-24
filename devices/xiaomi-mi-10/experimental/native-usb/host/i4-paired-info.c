#include <stdio.h>
#include <stdlib.h>

typedef struct idevice_private *idevice_t;
typedef struct lockdownd_client_private *lockdownd_client_t;
typedef void *plist_t;

extern int idevice_new_with_options(idevice_t *, const char *, int);
extern int idevice_free(idevice_t);
extern int lockdownd_client_new_with_handshake(idevice_t, lockdownd_client_t *, const char *);
extern int lockdownd_client_free(lockdownd_client_t);
extern int lockdownd_get_value(lockdownd_client_t, const char *, const char *, plist_t *);
extern const char *lockdownd_strerror(int);
extern void plist_get_string_val(plist_t, char **);
extern void plist_free(plist_t);

int main(void)
{
    idevice_t device = NULL;
    lockdownd_client_t client = NULL;
    int rc = idevice_new_with_options(&device, NULL, 1);
    if (rc != 0) {
        printf("USB device lookup: %d\n", rc);
        return 1;
    }
    rc = lockdownd_client_new_with_handshake(device, &client, "i4 USB paired metadata check");
    printf("Paired session: %d (%s)\n", rc, lockdownd_strerror(rc));
    if (rc == 0) {
        static const char *keys[] = {"ProductVersion", "ProductType", "HardwareModel"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            plist_t value = NULL;
            char *text = NULL;
            int status = lockdownd_get_value(client, NULL, keys[i], &value);
            if (value) {
                plist_get_string_val(value, &text);
                plist_free(value);
            }
            printf("%s: %s\n", keys[i], text ? text : lockdownd_strerror(status));
            free(text);
        }
        lockdownd_client_free(client);
    }
    idevice_free(device);
    return rc != 0;
}
