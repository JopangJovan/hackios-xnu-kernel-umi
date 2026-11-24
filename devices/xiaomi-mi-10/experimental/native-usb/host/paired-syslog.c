#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

typedef struct idevice_private *idevice_t;
typedef struct lockdownd_client_private *lockdownd_client_t;
typedef struct lockdownd_service_descriptor *lockdownd_service_descriptor_t;
typedef struct syslog_relay_client_private *syslog_relay_client_t;
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
extern int syslog_relay_client_new(idevice_t, lockdownd_service_descriptor_t, syslog_relay_client_t *);
extern int syslog_relay_client_free(syslog_relay_client_t);
extern int syslog_relay_receive_with_timeout(syslog_relay_client_t, char *, uint32_t, uint32_t *, unsigned int);

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) return 2;
    int duration = argc == 3 ? atoi(argv[2]) : 20;
    if (duration < 1 || duration > 180) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    idevice_t device = NULL;
    lockdownd_client_t client = NULL;
    lockdownd_service_descriptor_t service = NULL;
    syslog_relay_client_t relay = NULL;
    FILE *capture = NULL;
    int rc = idevice_new_with_options(&device, NULL, 1);
    if (rc) { printf("USB lookup: %d\n", rc); return 1; }
    rc = lockdownd_client_new_with_handshake(device, &client, "UMI paired USB log verification");
    printf("Paired session: %d (%s)\n", rc, lockdownd_strerror(rc));
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
    rc = lockdownd_start_service(client, "com.apple.syslog_relay", &service);
    printf("Start com.apple.syslog_relay: %d (%s)\n", rc, lockdownd_strerror(rc));
    if (rc) goto done;
    rc = syslog_relay_client_new(device, service, &relay);
    printf("Connect syslog relay: %d\n", rc);
    if (rc) goto done;
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL, 0600);
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
    if (bytes == 0) rc = -1;
done:
    if (capture && fclose(capture)) rc = -1;
    if (relay) syslog_relay_client_free(relay);
    if (service) lockdownd_service_descriptor_free(service);
    if (client) lockdownd_client_free(client);
    idevice_free(device);
    return rc ? 1 : 0;
}
