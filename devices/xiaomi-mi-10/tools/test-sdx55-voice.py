#!/usr/bin/env python3
"""Exercise the UMI voice worker and QMI wire format with a simulated modem."""
from pathlib import Path
import subprocess
import tempfile

DEV = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd/dev"
CODE = r'''
#include <assert.h>
#include <string.h>
#include "umi_sdx55_voice_wire.h"

#define QMI_RESPONSE 2
#define QMI_INDICATION 4
#define NSEC_PER_SEC 1000000000u
#define CELL_NAS 0
struct umi_mhi { bool poisoned; };
struct sdx55_cell_endpoint { uint32_t service, port, node, generation; };
static struct sdx55_cell_endpoint sdx55_services[] = {{3, 53, 3, 1}};
static struct { struct sdx55_cell_status cell; } sdx55_query;
static struct { unsigned nas_subscription; } cell_view = {2};
static struct { unsigned rat; } sdx55_radio = {5};
static uint32_t umi_sdx55_dms_node = 3;
static unsigned cell_status_lock;
static bool enabled, ims_registered = true, ims_rejected, cs_registered, lost_ack, wrong_number, reject;
static uint8_t call_state, direction = 1;
static unsigned sent, dials, hangups, dial_type;
static unsigned audio_connections;
static bool audio_connected;
static unsigned audio_route;
static bool reject_route;
static uint64_t now = 1;
static const char test_number[] = "+12025550123";

static void lck_mtx_lock(unsigned *lock) { assert(!(*lock)++); }
static void lck_mtx_unlock(unsigned *lock) { assert((*lock)-- == 1); }
static void hakios_tl(const char *format, ...) { assert(!strstr(format, test_number)); }
static uint64_t mach_absolute_time(void) { return now; }
static void clock_interval_to_deadline(unsigned delay, unsigned scale, uint64_t *end) {
    assert(scale == NSEC_PER_SEC);
    *end = now + delay;
}
static bool PE_parse_boot_argn(const char *name, uint32_t *value, size_t size) {
    assert(!strcmp(name, "sdx55_voice") && size == sizeof(*value));
    *value = enabled;
    return enabled;
}
static int umi_sdx55_poll_loc(struct umi_mhi *host, unsigned delay) { return 0; }
static int sdx55_flush_flow_acks(struct umi_mhi *host) { return 0; }
static void sdx55_ims_dcm_tick(struct umi_mhi *host, unsigned subscription) {}
static void sdx55_ims_dcm_close(struct umi_mhi *host) {}
static int sdx55_voice_audio_prepare(struct umi_mhi *host) { return 0; }
static void sdx55_voice_audio_observe_mailbox(const char *when) {}
static void sdx55_voice_audio_start(unsigned subscription) { audio_route = UMI_VOICE_ROUTE_RECEIVER; }
static void sdx55_voice_audio_stop(void) { audio_connected = false; audio_route = UMI_VOICE_ROUTE_NONE; }
static unsigned sdx55_voice_audio_route(void) { return audio_route; }
static int sdx55_voice_audio_switch(unsigned route) {
    if (reject_route) return EIO;
    audio_route = route;
    return 0;
}
static bool microphone_muted;
static int sdx55_voice_audio_mute(bool muted) {
    assert(audio_connected);
    microphone_muted = muted;
    return 0;
}
static void sdx55_voice_audio_connected(void) {
    assert(!audio_connected);
    audio_connected = true;
    audio_connections++;
}
static int sdx55_cell_command(struct umi_mhi *host, const struct sdx55_cell_endpoint *endpoint,
    unsigned message, unsigned arg, void *result) {
    assert(endpoint->port == 53 && message == SDX55_NAS_GET_SERVING_SYSTEM);
    sdx55_query.cell.registration = sdx55_query.cell.cs_attach = cs_registered;
    return 0;
}
static int umi_mhi_send_qrtr_packet(struct umi_mhi *, const void *, size_t, unsigned, uint16_t);

struct sysctl_req { int p; const void *newptr; size_t newlen; };
#define SYSCTL_HANDLER_ARGS (void *oidp, void *arg1, int arg2, struct sysctl_req *req)
#define SYSCTL_OUT(req, view, len) ((void)(view), 0)
#define SYSCTL_IN(req, request, len) (memcpy(request, (req)->newptr, len), 0)
static unsigned kauth_cred_get(void) { return 501; }
static unsigned kauth_cred_getuid(unsigned uid) { return uid; }
static const char *cs_identity_get(int proc) {
    if (proc == 3 || proc == 4) return "com.apple.Preferences";
    if (proc == 5) return "com.apple.InCallService";
    return proc ? "com.apple.mobilephone" : "com.example.other";
}
static bool csproc_get_platform_binary(int proc) { return proc == 1 || proc == 3 || proc == 5; }
#include "umi_sdx55_voice.inc"

static size_t put_tlv(uint8_t *reply, size_t off, uint8_t tag, const void *bytes, uint8_t len) {
    reply[off] = tag; reply[off+1] = len; reply[off+2] = 0;
    memcpy(reply + off + 3, bytes, len);
    return off + 3 + len;
}

static int umi_mhi_send_qrtr_packet(struct umi_mhi *host, const void *data, size_t size, unsigned timeout, uint16_t event) {
    const uint32_t *qrtr = data;
    const uint8_t *wire = (const uint8_t *)(qrtr + 8);
    unsigned message = sdx55_cell_le16(wire + 3);
    assert(qrtr[6] == 3 && size == 32 + ((qrtr[5] + 3) & ~3u));
    uint8_t reply[128] = {2, wire[1], wire[2], wire[3], wire[4], 0, 0, 2,4,0,0,0,0,0};
    size_t end = 14;
    ++sent;
    if (qrtr[3] == SDX55_IMSS_PORT) {
        assert(qrtr[7] == 73);
        if (message == SDX55_IMSS_BIND) {
            const uint8_t binding[] = {1,4,0,1,0,0,0};
            assert(qrtr[5] == 14 && !memcmp(wire + 7, binding, sizeof(binding)));
        } else {
            assert(message == SDX55_IMSS_GET_ENABLED);
            end = put_tlv(reply, end, 0x11, (uint8_t[]){1}, 1);
            end = put_tlv(reply, end, 0x19, (uint8_t[]){0}, 1);
        }
    } else if (qrtr[3] == SDX55_IMSA_PORT) {
        assert(qrtr[7] == 71);
        if (message == SDX55_IMSA_BIND) {
            const uint8_t binding[] = {0x10,4,0,1,0,0,0};
            assert(qrtr[5] == 14 && !memcmp(wire + 7, binding, sizeof(binding)));
        } else if (message == SDX55_IMSA_GET_BIND) {
            const uint8_t value[] = {1,0,0,0};
            end = put_tlv(reply, end, 0x10, value, sizeof(value));
        } else if (ims_rejected) {
            assert(message == SDX55_IMSA_REGISTRATION || message == SDX55_IMSA_SERVICES);
            reply[10] = 1; reply[12] = 70;
        } else if (message == SDX55_IMSA_REGISTRATION) {
            uint8_t value[] = {ims_registered ? 2 : 0,0,0,0};
            end = put_tlv(reply, end, 0x12, value, sizeof(value));
        } else {
            assert(message == SDX55_IMSA_SERVICES);
            const uint8_t available[] = {2,0,0,0}, wwan[] = {1,0,0,0};
            end = put_tlv(reply, end, 0x11, available, sizeof(available));
            end = put_tlv(reply, end, 0x14, wwan, sizeof(wwan));
        }
    } else {
        assert(qrtr[3] == SDX55_VOICE_PORT && qrtr[7] == 46);
        if (message == SDX55_VOICE_BIND) {
            const uint8_t binding[] = {1,1,0,1};
            assert(qrtr[5] == 11 && !memcmp(wire + 7, binding, sizeof(binding)));
        } else if (message == SDX55_VOICE_REGISTER) {
            const uint8_t speech_codec_events[] = {0x15,1,0,1};
            assert(qrtr[5] == 11 && !memcmp(wire + 7, speech_codec_events, sizeof(speech_codec_events)));
        } else if (message == SDX55_VOICE_DIAL) {
            size_t len = strlen(test_number);
            assert(wire[7] == 1 && sdx55_cell_le16(wire + 8) == len && !memcmp(wire + 10, test_number, len));
            assert(wire[10 + len] == 0x10 && sdx55_cell_le16(wire + 11 + len) == 1);
            assert(wire[13 + len] == 0);
            dial_type = qrtr[5] > 14 + len ? SDX55_DIAL_AUTOMATIC : 0;
            if (dial_type == 2) {
                const uint8_t media[] = {0x16,4,0,1,0,0,0, 0x18,8,0,3,0,0,0,0,0,0,0,
                    0x19,8,0,0,0,0,0,0,0,0,0, 0x22,1,0,0, 0x23,4,0,0,0,0,0};
                assert(qrtr[5] == 14 + len + sizeof(media));
                assert(!memcmp(wire + 14 + len, media, sizeof(media)));
            } else assert(qrtr[5] == 14 + len);
            ++dials;
            if (reject) {
                reply[10] = 1; reply[12] = 37;
                const uint8_t reason[] = {0x39,0x01};
                end = put_tlv(reply, end, 0x14, reason, sizeof(reason));
            } else {
                call_state = 5;
                if (lost_ack) return 0;
                end = put_tlv(reply, end, 0x10, (uint8_t[]){7}, 1);
            }
        } else if (message == SDX55_VOICE_END) {
            assert(wire[7] == 1 && wire[10] == 7 && direction == 1);
            ++hangups;
            call_state = 9;
            end = put_tlv(reply, end, 0x10, (uint8_t[]){7}, 1);
        } else {
            assert(message == SDX55_VOICE_GET_ALL);
            if (call_state) {
                const uint8_t record[] = {1,7,call_state,2,direction,4,0,0};
                uint8_t number[4 + sizeof(test_number) - 1] = {1,7,0,sizeof(test_number) - 1};
                memcpy(number + 4, test_number, sizeof(test_number) - 1);
                if (wrong_number) number[sizeof(number) - 1] = '9';
                end = put_tlv(reply, end, 0x10, record, sizeof(record));
                end = put_tlv(reply, end, 0x11, number, sizeof(number));
                if (call_state == 9) end = put_tlv(reply, end, 0x18, (uint8_t[]){1,7,0x19,0}, 4);
            }
        }
    }
    reply[5] = end - 7;
    sdx55_voice_receive(qrtr[3], 9, qrtr[7], reply, end);
    sdx55_voice_receive(qrtr[3], 3, qrtr[7] + 1, reply, end);
    ++reply[1];
    sdx55_voice_receive(qrtr[3], 3, qrtr[7], reply, end);
    --reply[1];
    assert(!voice_query.done);
    sdx55_voice_receive(qrtr[3], 3, qrtr[7], reply, end);
    return 0;
}

static int request(int proc, unsigned action, const char *number) {
    struct umi_voice_request value = {.size=sizeof(value), .version=UMI_VOICE_VERSION, .action=action};
    if (number) strcpy(value.number, number);
    struct sysctl_req req = {proc, &value, sizeof(value)};
    return sdx55_voice_sysctl(NULL, NULL, 0, &req);
}

static int request_route(int proc, uint32_t route) {
    struct sysctl_req req = {proc, &route, sizeof(route)};
    return sdx55_voice_route_sysctl(NULL, NULL, 0, &req);
}

static int request_mute(int proc, uint32_t mute) {
    struct sysctl_req req = {proc, &mute, sizeof(mute)};
    return sdx55_voice_mute_sysctl(NULL, NULL, 0, &req);
}

int main(void) {
    struct umi_mhi host = {0};
    uint8_t wire[64];
    assert(sdx55_voice_request(9, 0x44, 1, 1, NULL, wire, sizeof(wire)) == 11 && wire[10] == 0);
    assert(!sdx55_voice_request(9, 0x20, 1, 2, "112", wire, sizeof(wire)));
    assert(!sdx55_voice_request(9, 0x20, 1, 2, "+10086", wire, sizeof(wire)));
    assert(sdx55_voice_request(9, 0x20, 1, 2, "10086", wire, sizeof(wire)) == 59);
    assert(!memcmp(wire + 10, "10086", 5));
    assert(!sdx55_voice_request(9, 0x20, 1, 2, "+10010", wire, sizeof(wire)));
    assert(!sdx55_voice_request(9, 0x20, 1, 2, "10011", wire, sizeof(wire)));
    assert(sdx55_voice_request(9, 0x20, 1, 2, "10010", wire, sizeof(wire)) == 59);
    assert(wire[8] == 5 && wire[9] == 0 && !memcmp(wire + 10, "10010", 5));
    assert(!sdx55_voice_request(9, 0x20, 1, 2, test_number, wire, 15));
    struct sdx55_voice_result parsed;
    const uint8_t invalid[] = {4,0,0,0x2e,0,4,0,1,2,0,0};
    assert(sdx55_voice_decode(9, 0x2e, 0, invalid, sizeof(invalid), NULL, &parsed) == EPROTO);
    const uint8_t legacy[] = {2,1,0,0x20,0,11,0,2,4,0,0,0,0,0,0x10,1,0,1};
    assert(!sdx55_voice_decode(33, 0x20, 1, legacy, sizeof(legacy), NULL, &parsed) && parsed.value == 2);
    const uint8_t settings[] = {2,1,0,0x90,0,15,0,2,4,0,0,0,0,0,0x11,1,0,1,0x19,1,0,0};
    assert(!sdx55_voice_decode(18, 0x90, 1, settings, sizeof(settings), NULL, &parsed));
    assert(parsed.ims_enabled == 0 && parsed.volte_enabled == 1);

    sdx55_voice_announce(4, 9, 2, 3, 46);
    sdx55_voice_announce(4, 33, 1, 3, 71);
    sdx55_voice_announce(4, 18, 1, 3, 73);
    sdx55_voice_begin(); sdx55_voice_tick(&host);
    assert(!sent && request(1, UMI_VOICE_DIAL, test_number) == ENXIO);
    enabled = true;
    sdx55_voice_begin(); sdx55_voice_tick(&host);
    assert(voice_view.state == UMI_VOICE_READY && voice_view.ims_registration == 2 && sent == 8);
    assert(request_route(1, UMI_VOICE_ROUTE_SPEAKER) == ENXIO);
    assert(request_mute(1, 1) == ENXIO);
    assert(request(0, UMI_VOICE_DIAL, test_number) == EPERM);
    assert(request(2, UMI_VOICE_DIAL, test_number) == EPERM);
    struct sysctl_req prefs = {.p=3};
    assert(!sdx55_voice_sysctl(NULL, NULL, 0, &prefs));
    prefs.p = 4;
    assert(sdx55_voice_sysctl(NULL, NULL, 0, &prefs) == EPERM);
    assert(request(3, UMI_VOICE_DIAL, test_number) == EPERM);
    assert(request(3, UMI_VOICE_END, NULL) == EPERM);
    assert(request(5, UMI_VOICE_DIAL, test_number) == EPERM);
    assert(request(1, UMI_VOICE_DIAL, "112") == EINVAL);
    assert(!request(1, UMI_VOICE_DIAL, test_number));
    assert(request(1, UMI_VOICE_DIAL, test_number) == EBUSY);
    assert(!request(1, UMI_VOICE_END, NULL));
    sdx55_voice_tick(&host);
    assert(!dials && voice_view.state == UMI_VOICE_ENDED);

    call_state = 3; direction = 2;
    assert(!request(1, UMI_VOICE_DIAL, test_number)); sdx55_voice_tick(&host);
    assert(!dials && voice_view.error == EBUSY);
    call_state = 0; direction = 1;
    assert(!request(1, UMI_VOICE_DIAL, test_number)); sdx55_voice_tick(&host);
    assert(dials == 1 && dial_type == 2 && voice_view.state == UMI_VOICE_DIALING);
    now += 5; sdx55_voice_tick(&host);
    assert(voice_view.state == UMI_VOICE_RINGING && !audio_connected);
    const uint8_t active[] = {4,0,0,0x2e,0,11,0,1,8,0,1,7,3,2,1,4,0,0};
    sdx55_voice_receive(SDX55_VOICE_PORT, 3, 46, active, sizeof(active));
    sdx55_voice_tick(&host);
    assert(voice_view.state == UMI_VOICE_ACTIVE && audio_connected && audio_connections == 1);
    sdx55_voice_receive(SDX55_VOICE_PORT, 3, 46, active, sizeof(active));
    sdx55_voice_tick(&host);
    assert(voice_view.state == UMI_VOICE_ACTIVE && audio_connections == 1);
    assert(voice_route_view.route == UMI_VOICE_ROUTE_RECEIVER);
    assert(request_route(0, UMI_VOICE_ROUTE_SPEAKER) == EPERM);
    assert(request_route(1, UMI_VOICE_ROUTE_NONE) == EINVAL);
    assert(request_mute(5, 1) == EPERM);
    assert(request_mute(1, 2) == EINVAL);
    assert(!request_mute(1, 1));
    assert(request_route(5, UMI_VOICE_ROUTE_SPEAKER) == EBUSY);
    sdx55_voice_tick(&host);
    assert(voice_mute_view.muted && microphone_muted && !voice_mute_view.pending);
    assert(!request_mute(1, 0));
    sdx55_voice_tick(&host);
    assert(!voice_mute_view.muted && !microphone_muted && !voice_mute_view.pending);
    assert(!request_route(5, UMI_VOICE_ROUTE_SPEAKER));
    assert(voice_route_view.pending == UMI_VOICE_ROUTE_SPEAKER);
    assert(request_route(1, UMI_VOICE_ROUTE_RECEIVER) == EBUSY);
    sdx55_voice_tick(&host);
    assert(voice_route_view.route == UMI_VOICE_ROUTE_SPEAKER && !voice_route_view.pending);
    reject_route = true;
    assert(!request_route(1, UMI_VOICE_ROUTE_RECEIVER)); sdx55_voice_tick(&host);
    assert(voice_route_view.route == UMI_VOICE_ROUTE_SPEAKER && voice_route_view.error == EIO);
    reject_route = false;
    assert(!request_route(1, UMI_VOICE_ROUTE_RECEIVER));
    assert(!request(1, UMI_VOICE_END, NULL)); sdx55_voice_tick(&host);
    assert(hangups == 1 && voice_view.state == UMI_VOICE_ENDING);
    now++; sdx55_voice_tick(&host);
    assert(voice_view.state == UMI_VOICE_ENDED && voice_view.end_reason == 0x19 && !audio_connected);
    assert(voice_route_view.route == UMI_VOICE_ROUTE_NONE && !voice_route_view.pending);

    lost_ack = true;
    assert(!request(1, UMI_VOICE_DIAL, test_number)); sdx55_voice_tick(&host);
    assert(dials == 2 && voice_view.state == UMI_VOICE_UNCERTAIN);
    wrong_number = true;
    assert(!request(1, UMI_VOICE_END, NULL)); sdx55_voice_tick(&host);
    assert(hangups == 1 && dials == 2 && !voice_worker.owned);
    wrong_number = false;
    now++; sdx55_voice_tick(&host);
    assert(voice_worker.owned && voice_view.state == UMI_VOICE_RINGING && dials == 2);
    sdx55_voice_announce(5, 9, 2, 3, 46);
    sdx55_voice_tick(&host);
    assert(!voice_worker.owned && hangups == 1 && voice_view.state == UMI_VOICE_UNAVAILABLE);

    lost_ack = false; ims_registered = false; call_state = 0;
    sdx55_voice_announce(4, 9, 2, 3, 46);
    now += 5; sdx55_voice_tick(&host);
    assert(voice_view.state == UMI_VOICE_READY && voice_view.ims_registration == 0);
    sdx55_radio.rat = 0;
    assert(!request(1, UMI_VOICE_DIAL, test_number)); sdx55_voice_tick(&host);
    assert(dials == 2 && voice_view.state == UMI_VOICE_FAILED && voice_view.error == ENETUNREACH);
    sdx55_radio.rat = 5; reject = true;
    assert(!request(1, UMI_VOICE_DIAL, test_number)); sdx55_voice_tick(&host);
    assert(dials == 3 && dial_type == 2 && voice_view.qmi_error == 37);
    ims_rejected = true;
    now += 12; sdx55_voice_tick(&host);
    assert(!request(1, UMI_VOICE_DIAL, test_number)); sdx55_voice_tick(&host);
    assert(dials == 4 && dial_type == 2 && voice_view.qmi_error == 37);
    cs_registered = true;
    assert(!request(1, UMI_VOICE_DIAL, test_number)); sdx55_voice_tick(&host);
    assert(dials == 5 && !dial_type && voice_view.state == UMI_VOICE_FAILED && voice_view.qmi_error == 37 && voice_view.end_reason == 0x139);
    reject = false;
    assert(!request(1, UMI_VOICE_DIAL, test_number)); sdx55_voice_tick(&host);
    assert(dials == 6 && !dial_type);
    sdx55_voice_finish(&host);
    assert(hangups == 2 && !voice_worker.armed && voice_view.state == UMI_VOICE_UNAVAILABLE);
}
'''

with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / "voice.c"
    binary = source.with_suffix("")
    source.write_text(CODE)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter",
                    "-I", str(DEV), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: SDX55 call states, hangup ownership, and in-call audio route requests")
