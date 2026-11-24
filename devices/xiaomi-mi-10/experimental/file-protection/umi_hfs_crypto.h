/* UMI-only per-file format: complete 512-byte XTS data units.
 * Requires the audited constant-time AES implementation from crypt-volume.
 * The existing authenticated outer test volume supplies integrity. */
#ifndef UMI_HFS_CRYPTO_H
#define UMI_HFS_CRYPTO_H
static void umi_hfs_xts_unit(const struct umi_ct_key *data,const struct umi_ct_key *tw,
                          int decrypt,uint64_t sector,const uint8_t *in,uint8_t *out) {
    uint8_t tweak[16]={0},block[16];
    for(unsigned i=0;i<8;i++)tweak[i]=(uint8_t)(sector>>(8*i));
    umi_ct_block(tw,0,tweak,tweak);
    for(unsigned n=0;n<512;n+=16) {
        for(unsigned i=0;i<16;i++)block[i]=in[n+i]^tweak[i];
        umi_ct_block(data,decrypt,block,block);
        for(unsigned i=0;i<16;i++)out[n+i]=block[i]^tweak[i];
        unsigned carry=0;
        for(unsigned i=0;i<16;i++){unsigned next=tweak[i]>>7;tweak[i]=(uint8_t)((tweak[i]<<1)|carry);carry=next;}
        tweak[0]^=(uint8_t)(0x87u & (0u-carry));
    }
    umi_ct_wipe(tweak,sizeof(tweak));umi_ct_wipe(block,sizeof(block));
}
#endif
