"""Exercise WMS framing and 3GPP SMS encoding and decoding on the host."""
from pathlib import Path
import os
import subprocess
import tempfile

device = Path(__file__).resolve().parents[1]
bsd = device / "overlays/xnu/files/bsd"
userland = device / "overlays/userland/src"

source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/errno.h>
#include "umi_sdx55_sms_wire.h"
#include "umi_sms_submit.h"

int main(void)
{
    const uint8_t english[] = {
        4, 5, 0x81, 0x01, 0x00, 0xf1, 0, 0, 0x62, 0x11, 0x41, 0x02, 0x01, 0, 0x23,
        5, 0xe8, 0x32, 0x9b, 0xfd, 0x06
    };
    const uint8_t chinese[] = {
        4, 4, 0x81, 0x01, 0x01, 0, 8, 0x62, 0x11, 0x41, 0x02, 0x01, 0, 0x23,
        6, 0x6d, 0x4b, 0x8b, 0xd5, 0, 0x31
    };
    const uint8_t multipart[] = {
        0x44, 4, 0x81, 0x01, 0x01, 0, 8, 0x62, 0x11, 0x41, 0x02, 0x01, 0, 0x23,
        9, 6, 0x08, 4, 0x01, 0x23, 2, 1, 0x6d, 0x4b
    };
    const uint8_t gsm_udh[] = {
        0x44, 4, 0x81, 0x01, 0x01, 0, 0, 0x62, 0x11, 0x41, 0x02, 0x01, 0, 0x23,
        10, 5, 0x00, 3, 0x42, 2, 1, 0x82, 0x1b, 0x0a
    };
    const uint16_t hello[] = {'h', 'e', 'l', 'l', 'o'};
    struct umi_sms_deliver sms;
    assert(umi_sms_deliver_decode(english, sizeof(english), &sms));
    assert(sms.sender_length == 5 && sms.sender[0] == '1' && sms.sender[4] == '1');
    assert(sms.text_length == 5 && !memcmp(sms.text, hello, sizeof(hello)));
    assert(umi_sms_deliver_decode(chinese, sizeof(chinese), &sms));
    assert(sms.text_length == 3 && sms.text[0] == 0x6d4b && sms.text[1] == 0x8bd5 && sms.text[2] == '1');
    assert(umi_sms_deliver_decode(multipart, sizeof(multipart), &sms));
    assert(sms.parts == 2 && sms.part == 1 && sms.reference == 0x123 && sms.long_reference);
    assert(sms.text_length == 1 && sms.text[0] == 0x6d4b);
    assert(umi_sms_deliver_decode(gsm_udh, sizeof(gsm_udh), &sms));
    assert(sms.parts == 2 && sms.reference == 0x42 && sms.text_length == 2);
    assert(sms.text[0] == 'A' && sms.text[1] == '^');
    for (size_t length = 0; length < sizeof(chinese); length++)
        assert(!umi_sms_deliver_decode(chinese, length, &sms));
    uint8_t invalid[sizeof(chinese)];
    memcpy(invalid, chinese, sizeof(invalid));
    invalid[14] = 0xff;
    assert(!umi_sms_deliver_decode(invalid, sizeof(invalid), &sms));

    uint8_t indication[] = {
        4, 0x26, 0, 1, 0, 18, 0, 0x11, 11, 0, 0, 0x78, 0x56, 0x34, 0x12, 6, 3, 0, 4, 5, 6, 0x16, 1, 0, 1
    };
    struct sdx55_sms_event event;
    uint8_t stored[7 + sizeof(english)] = {0, 6, sizeof(english) + 3, 0, 2, 0x91, 0x21};
    memcpy(stored + 7, english, sizeof(english));
    assert(!sdx55_sms_stored_tpdu(stored, sizeof(stored), &event));
    assert(event.length == sizeof(english) && !memcmp(event.pdu, english, sizeof(english)));
    stored[4] = 0xff;
    assert(sdx55_sms_stored_tpdu(stored, sizeof(stored), &event) == EINVAL);
    assert(!sdx55_sms_event_decode(indication, sizeof(indication), &event));
    assert(event.ack && event.transfer && event.ims && event.length == 3 && event.transaction == 0x12345678);
    assert(sdx55_sms_event_decode(indication, sizeof(indication) - 1, &event) == EINVAL);
    indication[16] = 0xff;
    assert(sdx55_sms_event_decode(indication, sizeof(indication), &event) == EINVAL);
    indication[16] = 3;
    assert(!sdx55_sms_event_decode(indication, sizeof(indication), &event));
    uint8_t request[68];
    assert(sdx55_sms_request(SDX55_WMS_BIND, 1, 1, NULL, NULL, request, sizeof(request)) == 11);
    assert(request[7] == 1 && request[8] == 1 && request[10] == 0);
    assert(sdx55_sms_request(SDX55_WMS_BIND, 1, 2, NULL, NULL, request, sizeof(request)) == 11);
    assert(request[10] == 1);
    size_t n = sdx55_sms_request(SDX55_WMS_ACK, 0x13, 1, NULL, &event, request, sizeof(request));
    const uint8_t ims_ack[] = {0, 0x13, 0, 0x37, 0, 13, 0, 1, 6, 0, 0x78, 0x56, 0x34, 0x12, 1, 1, 0x12, 1, 0, 1};
    assert(n == sizeof(ims_ack) && !memcmp(request, ims_ack, n));
    assert(sdx55_sms_request(SDX55_WMS_ACK, 0x13, 2, NULL, &event, request, sizeof(request)) == 25);
    assert(request[15] == 0 && request[23] == 0x16 && request[24] == 0xd3);

    const uint8_t routes[] = {3, 0, 0, 2, 0, 1, 0, 4, 1, 1, 0, 5, 0xff, 2};
    struct sdx55_sms_routes saved = {.status_reports = 1, .has_status_reports = true};
    assert(!sdx55_sms_routes_decode(routes, sizeof(routes), &saved));
    assert(sdx55_sms_routes_decode(routes, sizeof(routes) - 1, &saved) == EINVAL);
    struct sdx55_sms_routes receive = sdx55_sms_receive_routes(&saved);
    assert(receive.count == 3 && receive.entries[1].storage == 0xff && receive.entries[1].action == 2);
    assert(receive.entries[0].storage == 0 && receive.entries[0].action == 1);
    assert(saved.entries[1].storage == 1 && saved.entries[1].action == 1);
    assert(sdx55_sms_request(SDX55_WMS_SET_ROUTES, 6, 0, &saved, NULL, request, sizeof(request)) == 28);
    assert(!memcmp(request + 10, routes, sizeof(routes)) && request[24] == 0x10 && request[27] == 1);

    struct umi_sms_submit_plan plan;
    struct umi_sms_submission submit;
    assert(umi_sms_plan(hello, 5, &plan) && plan.gsm && plan.count == 1);
    assert(umi_sms_submit("+86123", hello, &plan, 0, 0, &submit));
    const uint8_t gsm_submit[] = {
        0, 1, 0, 5, 0x91, 0x68, 0x21, 0xf3, 0, 0, 5, 0xe8, 0x32, 0x9b, 0xfd, 0x06
    };
    assert(submit.length == sizeof(gsm_submit) && !memcmp(submit.pdu, gsm_submit, sizeof(gsm_submit)));
    const uint8_t send_header[] = {0, 7, 0, 0x20, 0, 26, 0, 1, 19, 0, 6, 16, 0};
    assert(sdx55_sms_send_request(&submit, 7, true, request, sizeof(request)) == 33);
    assert(!memcmp(request, send_header, sizeof(send_header)));
    assert(!memcmp(request + 13, gsm_submit, sizeof(gsm_submit)));
    assert(!memcmp(request + 29, (const uint8_t[]){0x13, 1, 0, 1}, 4));
    assert(sdx55_sms_send_request(&submit, 7, false, request, sizeof(request)) == 29 && request[5] == 22);
    submit.pdu[7] = 3;
    assert(!sdx55_sms_submit_valid(&submit));

    const uint16_t chinese_text[] = {0x6d4b, 0x8bd5, '1'};
    const uint8_t unicode_submit[] = {0, 1, 0, 4, 0x81, 1, 1, 0, 8, 6, 0x6d, 0x4b, 0x8b, 0xd5, 0, 0x31};
    assert(umi_sms_plan(chinese_text, 3, &plan) && !plan.gsm && plan.count == 1);
    assert(umi_sms_submit("1010", chinese_text, &plan, 0, 50, &submit));
    assert(sdx55_sms_submit_valid(&submit) && submit.length == sizeof(unicode_submit));
    assert(!memcmp(submit.pdu, unicode_submit, sizeof(unicode_submit)));
    assert(!umi_sms_submit("+1x", chinese_text, &plan, 0, 0, &submit));

    uint16_t text[160], decoded[160];
    for (unsigned i = 0; i < 160; i++) text[i] = 'a';
    assert(umi_sms_plan(text, 160, &plan) && plan.count == 1);
    assert(umi_sms_submit("12345678912345678912", text, &plan, 0, 0, &submit));
    assert(submit.length == UMI_SMS_SUBMIT_MAX && sdx55_sms_submit_valid(&submit));
    text[152] = '^'; /* Its escape pair must stay together. */
    assert(umi_sms_plan(text, 160, &plan) && plan.gsm && plan.count == 2 && plan.end[0] == 152);
    assert(umi_sms_submit("1000", text, &plan, 1, 0x42, &submit));
    assert(sdx55_sms_submit_valid(&submit));
    assert(!memcmp(submit.pdu + 10, (const uint8_t[]){5, 0, 3, 0x42, 2, 2}, 6));
    assert(umi_gsm_unpack(submit.pdu + 10, submit.length - 10, 49, submit.pdu[9] - 7, decoded) == 8);
    assert(!memcmp(decoded, text + 152, 8 * sizeof(text[0])));

    text[0] = 0x6d4b;
    text[66] = 0xd83d;
    text[67] = 0xde42;
    assert(umi_sms_plan(text, 72, &plan) && !plan.gsm && plan.count == 2 && plan.end[0] == 66);
    assert(umi_sms_submit("1000", text, &plan, 1, 0x42, &submit));
    assert(sdx55_sms_submit_valid(&submit));
    assert(!memcmp(submit.pdu + 16, (const uint8_t[]){0xd8, 0x3d, 0xde, 0x42}, 4));
    assert(!umi_sms_plan(text, 67, &plan));
}
'''

with tempfile.TemporaryDirectory() as tmp:
    test = Path(tmp) / "test-sms"
    subprocess.run(
        [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I", bsd, "-I", bsd / "dev", "-I", userland, "-xc", "-", "-o", test],
        input=source, text=True, check=True,
    )
    subprocess.run([test], check=True)
print("PASS: SDX55 SMS transfer, route restore, IMS ACK/Raw Send, GSM 7-bit and Unicode concatenation")
