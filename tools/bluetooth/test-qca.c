#include <assert.h>
#include <string.h>
#include "../../xnu/files/bsd/sys/hk_bluetooth.h"
int main(void)
{
    uint8_t nvm[] = {2,18,0,0, 17,0,6,0, 0,0,0,0,0,0,0,0, 0x15,0x11,0x22,0,0x2e,0};
    assert(hk_bt_qca6390_nvm_prepare(nvm,sizeof(nvm)) == 0);
    assert(nvm[16] == 0x15 && nvm[17] == 0 && nvm[18] == 0x22);
    /* A later malformed record must prevent even an earlier valid tag edit. */
    uint8_t bad[sizeof(nvm)+1]; memcpy(bad,nvm,sizeof(nvm));
    bad[1]++; bad[17]=0x11; bad[sizeof(nvm)]=0;
    assert(hk_bt_qca6390_nvm_prepare(bad,sizeof(bad)) < 0 && bad[17] == 0x11);
    nvm[6]=0xff;
    assert(hk_bt_qca6390_nvm_prepare(nvm,sizeof(nvm)) < 0);
    const uint8_t ack[] = {4,0x0e,5,1,0,0xfc,0,0x1e};
    assert(hk_bt_qca_tlv_ack(ack,sizeof(ack)));
    assert(!hk_bt_qca_tlv_ack(ack,sizeof(ack)-1));
    uint8_t wrong[sizeof(ack)]; memcpy(wrong,ack,sizeof(ack));
    wrong[7]=0x19; assert(!hk_bt_qca_tlv_ack(wrong,sizeof(wrong)));
    wrong[7]=0x1e; wrong[6]=1; assert(!hk_bt_qca_tlv_ack(wrong,sizeof(wrong)));
    assert(hk_bt_cc_status(ack,sizeof(ack),0x1001) < 0);
    return 0;
}
