#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct idevice_private *idevice_t;
typedef struct lockdownd_client_private *lockdownd_client_t;
typedef struct lockdownd_service_descriptor *lockdownd_service_descriptor_t;
typedef struct afc_client_private *afc_client_t;
typedef void *plist_t;
extern int idevice_new_with_options(idevice_t *, const char *, int);
extern int idevice_free(idevice_t);
extern int lockdownd_client_new_with_handshake(idevice_t, lockdownd_client_t *, const char *);
extern int lockdownd_client_free(lockdownd_client_t);
extern int lockdownd_get_value(lockdownd_client_t, const char *, const char *, plist_t *);
extern int lockdownd_start_service(lockdownd_client_t, const char *, lockdownd_service_descriptor_t *);
extern int lockdownd_service_descriptor_free(lockdownd_service_descriptor_t);
extern const char *lockdownd_strerror(int);
extern void plist_get_string_val(plist_t, char **);
extern void plist_free(plist_t);
extern int afc_client_new(idevice_t, lockdownd_service_descriptor_t, afc_client_t *);
extern int afc_client_free(afc_client_t);
extern int afc_read_directory(afc_client_t, const char *, char ***);
extern int afc_dictionary_free(char **);
extern int afc_file_open(afc_client_t, const char *, int, uint64_t *);
extern int afc_file_close(afc_client_t, uint64_t);
extern int afc_file_read(afc_client_t, uint64_t, char *, uint32_t, uint32_t *);

int main(int argc, char **argv)
{
    if ((argc != 2 && argc != 3) ||
        (strcmp(argv[1], "/DCIM") && strncmp(argv[1], "/DCIM/", 6) && strcmp(argv[1], "/PhotoData")) ||
        strstr(argv[1], "..")) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    idevice_t device = NULL;
    lockdownd_client_t client = NULL;
    lockdownd_service_descriptor_t service = NULL;
    afc_client_t afc = NULL;
    int rc = idevice_new_with_options(&device, NULL, 1);
    if (rc) { printf("USB lookup: %d\n", rc); return 1; }
    rc = lockdownd_client_new_with_handshake(device, &client, "UMI read-only photo verification");
    printf("Paired session: %d (%s)\n", rc, lockdownd_strerror(rc));
    if (rc) goto done;
    plist_t value = NULL;
    char *model = NULL;
    rc = lockdownd_get_value(client, NULL, "HardwareModel", &value);
    if (!rc && value) plist_get_string_val(value, &model);
    if (rc || !model || strcmp(model, "D22AP")) rc = -1;
    free(model);
    if (value) plist_free(value);
    if (rc) goto done;
    rc = lockdownd_start_service(client, "com.apple.afc", &service);
    printf("Start AFC: %d (%s)\n", rc, lockdownd_strerror(rc));
    if (rc) goto done;
    rc = afc_client_new(device, service, &afc);
    printf("Connect AFC: %d\n", rc);
    if (rc) goto done;
    if (argc == 2) {
        char **entries = NULL;
        rc = afc_read_directory(afc, argv[1], &entries);
        printf("Read directory %s: %d\n", argv[1], rc);
        if (!rc && entries) {
            unsigned n = 0;
            for (; entries[n]; ++n) if (n < 50) printf("  %s\n", entries[n]);
            printf("Directory entries: %u\n", n);
        }
        if (entries) afc_dictionary_free(entries);
    } else {
        uint64_t handle = 0;
        rc = afc_file_open(afc, argv[1], 1, &handle);
        printf("Open photo read-only: %d\n", rc);
        if (rc) goto done;
        int fd = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
        FILE *out = fd < 0 ? NULL : fdopen(fd, "wb");
        if (!out) { if (fd >= 0) close(fd); perror("local export"); rc = -1; }
        uint64_t total = 0;
        while (!rc && total < 64 * 1024 * 1024) {
            char buf[65536]; uint32_t bytes = 0;
            rc = afc_file_read(afc, handle, buf, sizeof(buf), &bytes);
            if (rc || bytes == 0) break;
            if (fwrite(buf, 1, bytes, out) != bytes) { rc = -1; break; }
            total += bytes;
        }
        if (total >= 64 * 1024 * 1024) rc = -1;
        if (out && fclose(out)) rc = -1;
        int closed = afc_file_close(afc, handle);
        printf("Photo read status: %d; close: %d; bytes: %llu\n", rc, closed, (unsigned long long)total);
        if (closed || !total) rc = -1;
    }
done:
    if (afc) afc_client_free(afc);
    if (service) lockdownd_service_descriptor_free(service);
    if (client) lockdownd_client_free(client);
    idevice_free(device);
    return rc ? 1 : 0;
}
