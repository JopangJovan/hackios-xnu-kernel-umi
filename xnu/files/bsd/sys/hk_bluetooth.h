/* Bluetooth H4 framing shared by the UMI UART transport and host checks.
 * QCA IBS bytes are control messages only between complete H4 packets. */
#ifndef HK_BLUETOOTH_H
#define HK_BLUETOOTH_H
#include <stdint.h>
#include <stddef.h>
#define HK_BT_H4_MAX 4096u
struct hk_bt_h4 {
    uint8_t bytes[HK_BT_H4_MAX];
    uint32_t used, wanted, header;
    int complete;
};
/* 0: incomplete, 1: packet ready, 2: QCA IBS byte, -1: bad type,
 * -2: packet exceeds the fixed bound. Transport must reset on framing errors.
 * Complete bytes remain valid until the next call. */
static inline int hk_bt_h4_push(struct hk_bt_h4 *p, uint8_t byte)
{
    if (p->complete) { p->used = p->wanted = p->header = 0; p->complete = 0; }
    if (p->used == 0) {
        if (byte >= 0xfc && byte <= 0xfe) return 2;
        switch (byte) {
        case 1: case 3: p->header = 4; break;
        case 2: case 5: p->header = 5; break;
        case 4: p->header = 3; break;
        default: return -1;
        }
        p->wanted = p->header;
    }
    if (p->used >= HK_BT_H4_MAX) return -2;
    p->bytes[p->used++] = byte;
    if (p->used == p->header) {
        uint32_t length;
        switch (p->bytes[0]) {
        case 1: case 3: length = p->bytes[3]; break;
        case 4: length = p->bytes[2]; break;
        default:
            length = p->bytes[3] | (uint32_t)p->bytes[4] << 8;
            if (p->bytes[0] == 5) length &= 0x3fff;
            break;
        }
        if (length > HK_BT_H4_MAX - p->header) {
            p->used = p->wanted = p->header = 0;
            return -2;
        }
        p->wanted = p->header + length;
    }
    if (p->used == p->wanted) { p->complete = 1; return 1; }
    return 0;
}
void umi_bluetooth_init(void);
#endif
