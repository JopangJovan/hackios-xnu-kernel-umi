/* Only the bounded encrypted fixture disk consumes these software file keys.
 * Never modify a plaintext buffer in place on a write. HFS owns the CPX lifetime
 * until biodone, and its lock callback drains I/O before clearing cached keys. */
#ifndef UMI_HFS_IO_H
#define UMI_HFS_IO_H
#include <sys/cprotect.h>
#include "umi_hfs_crypto.h"
static int umi_hfs_io(buf_t bp,bool reading,uint64_t disk_at,uint8_t *bytes,size_t n,uint8_t *bounce) {
    cpx_t cpx=bufattr_cpx(buf_attr(bp));
    if(!cpx)return uv_io(reading,disk_at,bytes,n,bounce);
    uint64_t off=bufattr_cpoff(buf_attr(bp));
    if(!n||(n%512)||(off%512)||off>UINT64_MAX-n||n>128*1024||
       cpx_key_len(cpx)!=64||cpx_is_sep_wrapped_key(cpx)||!cpx_use_offset_for_iv(cpx)||
       bufattr_rawencrypted(buf_attr(bp)))return ENOTSUP;
    uint8_t *cipher=kalloc_data(n,Z_WAITOK);if(!cipher)return ENOMEM;
    struct umi_ct_key data,tweak;int e=0;
    if(umi_ct_setkey(&data,(const uint8_t *)cpx_key(cpx),32)||
       umi_ct_setkey(&tweak,(const uint8_t *)cpx_key(cpx)+32,32))e=EINVAL;
    if(!e&&reading)e=uv_io(true,disk_at,cipher,n,bounce);
    if(!e){
        for(size_t i=0;i<n;i+=512)
            umi_hfs_xts_unit(&data,&tweak,reading,(off+i)/512,
                (reading?cipher:bytes)+i,(reading?bytes:cipher)+i);
        if(!reading)e=uv_io(false,disk_at,cipher,n,bounce);
    }
    umi_ct_wipe(&data,sizeof(data));umi_ct_wipe(&tweak,sizeof(tweak));
    umi_ct_wipe(cipher,n);kfree_data(cipher,n);return e;
}
#endif
