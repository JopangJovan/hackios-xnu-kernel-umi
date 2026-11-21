#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <CommonCrypto/CommonDigest.h>

typedef struct idevice_private *idevice_t;
typedef struct lockdownd_client_private *lockdownd_client_t;
typedef struct lockdownd_service_descriptor *lockdownd_service_descriptor_t;
typedef struct syslog_relay_client_private *syslog_relay_client_t;
typedef void *plist_t;
extern int idevice_new_with_options(idevice_t *, const char *, int);
extern int idevice_free(idevice_t);
extern int lockdownd_client_new(idevice_t, lockdownd_client_t *, const char *);
extern int lockdownd_start_session(lockdownd_client_t, const char *, char **, int *);
extern int usbmuxd_read_pair_record(const char *, char **, uint32_t *);
extern void plist_from_bin(const char *, uint32_t, plist_t *);
extern void plist_from_xml(const char *, uint32_t, plist_t *);
extern plist_t plist_dict_get_item(plist_t, const char *);
typedef struct diagnostics_relay_client_private *diagnostics_relay_client_t;
extern int diagnostics_relay_client_new(idevice_t,lockdownd_service_descriptor_t,diagnostics_relay_client_t*);
extern int diagnostics_relay_client_free(diagnostics_relay_client_t);
extern int diagnostics_relay_restart(diagnostics_relay_client_t,int);

extern void plist_get_string_val(plist_t, char **);
extern void plist_free(plist_t);

static int read_existing(const char *device, char **host, unsigned char digest[32])
{
    char *bytes = NULL;
    uint32_t size = 0;
    int rc = usbmuxd_read_pair_record(device, &bytes, &size);
    if (rc || !bytes || !size) {
        free(bytes);
        return -1;
    }
    CC_SHA256(bytes, size, digest);
    plist_t record = NULL;
    if (size >= 8 && memcmp(bytes, "bplist00", 8) == 0)
        plist_from_bin(bytes, size, &record);
    else
        plist_from_xml(bytes, size, &record);
    for (volatile char *p = bytes; size--; ++p) *p = 0;
    free(bytes);
    if (!record) return -1;
    plist_t item = plist_dict_get_item(record, "HostID");
    if (item) plist_get_string_val(item, host);
    plist_free(record);
    return *host && **host ? 0 : -1;
}

extern int lockdownd_client_free(lockdownd_client_t);
extern int lockdownd_get_value(lockdownd_client_t, const char *, const char *, plist_t *);
extern int lockdownd_start_service(lockdownd_client_t, const char *, lockdownd_service_descriptor_t *);
extern int lockdownd_service_descriptor_free(lockdownd_service_descriptor_t);
extern const char *lockdownd_strerror(int);
extern int syslog_relay_client_new(idevice_t, lockdownd_service_descriptor_t, syslog_relay_client_t *);
extern int syslog_relay_client_free(syslog_relay_client_t);
extern int syslog_relay_receive_with_timeout(syslog_relay_client_t, char *, uint32_t, uint32_t *, unsigned int);

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: existing-pair DEVICE_ID info|restart|OUTPUT_PATH SECONDS\n");
        return 2;
    }
    const int restart = !strcmp(argv[2], "restart");
    const int info = !strcmp(argv[2], "info");
    int duration = atoi(argv[3]);
    if (!restart && !info && (duration < 1 || duration > 180)) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    idevice_t device = NULL;
    lockdownd_client_t client = NULL;
    lockdownd_service_descriptor_t service = NULL;
    syslog_relay_client_t relay = NULL;
    FILE *capture = NULL;
    unsigned char pair_before[32], pair_after[32];
    char *host = NULL, *after = NULL;
    int ssl = 0;
    if (read_existing(argv[1], &host, pair_before)) { free(host); puts("No existing pair record"); return 1; }
    int rc = idevice_new_with_options(&device, argv[1], 1);
    if (rc) { printf("USB lookup: %d\n", rc); free(host); return 1; }
    rc = lockdownd_client_new(device, &client, "UMI existing trust verification");
    if (!rc) rc = lockdownd_start_session(client, host, NULL, &ssl);
    printf("Existing paired session: %d (%s), SSL=%d\n", rc, lockdownd_strerror(rc), ssl);
    if (!rc && !ssl) rc = -1;
    if (rc) goto done;
    const char *const keys[] = {"ProductVersion", "ProductType", "HardwareModel"};
    for (size_t n = 0; n < sizeof(keys)/sizeof(keys[0]); ++n) {
        plist_t value = NULL;
        char *text = NULL;
        rc = lockdownd_get_value(client, NULL, keys[n], &value);
        if (!rc && value) plist_get_string_val(value, &text);
        printf("%s: %s\n", keys[n], text ? text : lockdownd_strerror(rc));
        const char *expected[] = {"16.5", "iPhone10,3", "D22AP"};
        if (!text || strcmp(text, expected[n])) rc = -1;
        free(text);
        if (value) plist_free(value);
        if (rc) goto done;
    }
    if (info || restart) {
        if (read_existing(argv[1], &after, pair_after) || memcmp(pair_before, pair_after, sizeof(pair_before))) {
            rc = -1;
            goto done;
        }
        puts("Stored pair record bytes unchanged");
        if (info) goto done;
        diagnostics_relay_client_t diagnostics = NULL;
        rc = lockdownd_start_service(client, "com.apple.mobile.diagnostics_relay", &service);
        if (!rc) rc = diagnostics_relay_client_new(device, service, &diagnostics);
        if (!rc) rc = diagnostics_relay_restart(diagnostics, 0);
        printf("Native restart: %d\n", rc);
        if (diagnostics) diagnostics_relay_client_free(diagnostics);
        goto done;
    }
    rc = lockdownd_start_service(client, "com.apple.syslog_relay", &service);
    printf("Start com.apple.syslog_relay: %d (%s)\n", rc, lockdownd_strerror(rc));
    if (rc) goto done;
    rc = syslog_relay_client_new(device, service, &relay);
    printf("Connect syslog relay: %d\n", rc);
    if (rc) goto done;
    int fd = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd == -1) { perror("capture"); rc = -1; goto done; }
    capture = fdopen(fd, "wb");
    if (!capture) { close(fd); rc = -1; goto done; }
    const double until = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e9 + duration;
    unsigned long bytes = 0, lines = 0;
    do {
        char buffer[8192];
        uint32_t received = 0;
        int status = syslog_relay_receive_with_timeout(relay, buffer, sizeof(buffer), &received, 1000);
        if (received) {
            if (fwrite(buffer, 1, received, capture) != received) { rc = -1; break; }
            bytes += received;
            for (uint32_t i = 0; i < received; ++i) if (buffer[i] == '\n') ++lines;
        }
        if (status && status != -5 && status != -4) { printf("Receive status: %d\n", status); rc = status; break; }
    } while (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e9 < until);
    printf("Native USB log capture: %lu bytes, %lu lines\n", bytes, lines);
    if (!bytes || read_existing(argv[1], &after, pair_after) ||
        memcmp(pair_before, pair_after, sizeof(pair_before))) rc = -1;
    if (!rc) puts("Stored pair record bytes unchanged");
done:
    free(host);
    free(after);
    if (capture && fclose(capture)) rc = -1;
    if (relay) syslog_relay_client_free(relay);
    if (service) lockdownd_service_descriptor_free(service);
    if (client) lockdownd_client_free(client);
    idevice_free(device);
    return rc ? 1 : 0;
}
