#include <assert.h>
#include <string.h>
#include "../../xnu/files/bsd/sys/hk_bluetooth.h"
int main(void)
{
    struct hk_bt_h4 p = {0};
    /* Fragmented Command Complete for HCI Reset; IBS is not an event. */
    const uint8_t event[] = {4, 0x0e, 4, 1, 3, 0x0c, 0};
    assert(hk_bt_h4_push(&p, 0xfd) == 2);
    for (size_t i = 0; i < sizeof(event); i++)
        assert(hk_bt_h4_push(&p, event[i]) == (i+1 == sizeof(event) ? 1 : 0));
    assert(p.used == sizeof(event) && !memcmp(p.bytes, event, sizeof(event)));
    /* Vendor payload bytes resembling IBS must remain payload. */
    const uint8_t vendor[] = {4, 0xff, 3, 0xfc, 0xfd, 0xfe};
    for (size_t i = 0; i < sizeof(vendor); i++)
        assert(hk_bt_h4_push(&p, vendor[i]) == (i+1 == sizeof(vendor) ? 1 : 0));
    assert(!memcmp(p.bytes, vendor, sizeof(vendor)));
    assert(hk_bt_h4_push(&p, 0) == -1);
    const uint8_t oversized[] = {2, 0, 0, 0xff, 0xff};
    for (size_t i = 0; i < sizeof(oversized); i++)
        assert(hk_bt_h4_push(&p, oversized[i]) == (i == 4 ? -2 : 0));
    assert(p.used == 0);
    /* ISO's top length bits are flags, not payload length. */
    const uint8_t iso[] = {5, 0, 0, 1, 0xc0, 0x5a};
    for (size_t i = 0; i < sizeof(iso); i++)
        assert(hk_bt_h4_push(&p, iso[i]) == (i == 5 ? 1 : 0));
    /* Exact buffer boundary, followed by another complete event. */
    const uint8_t acl[] = {2, 0, 0, 0xfb, 0x0f};
    for (size_t i = 0; i < sizeof(acl); i++) assert(hk_bt_h4_push(&p, acl[i]) == 0);
    for (unsigned i = 0; i < HK_BT_H4_MAX - sizeof(acl); i++)
        assert(hk_bt_h4_push(&p, 0x5a) == (i == HK_BT_H4_MAX - sizeof(acl) - 1 ? 1 : 0));
    assert(p.used == HK_BT_H4_MAX);
    for (size_t i = 0; i < sizeof(event); i++)
        assert(hk_bt_h4_push(&p, event[i]) == (i+1 == sizeof(event) ? 1 : 0));
    return 0;
}
