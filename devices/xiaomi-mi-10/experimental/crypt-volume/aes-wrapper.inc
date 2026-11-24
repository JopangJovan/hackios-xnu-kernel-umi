struct umi_ct_key { uint32_t sk[120]; unsigned rounds; };
static int umi_ct_setkey(struct umi_ct_key *ctx, const uint8_t *key, size_t len) {
    uint32_t compact[60]={0};
    ctx->rounds=br_aes_ct_keysched(compact,key,len);
    if(ctx->rounds)br_aes_ct_skey_expand(ctx->sk,ctx->rounds,compact);
    umi_ct_wipe(compact,sizeof(compact));return ctx->rounds?0:-1;
}
static void umi_ct_block(const struct umi_ct_key *ctx,int decrypt,const uint8_t in[16],uint8_t out[16]) {
    uint32_t q[8]={0};
    for(unsigned i=0;i<4;i++)q[i*2]=br_dec32le(in+i*4);
    br_aes_ct_ortho(q);
    if(decrypt)br_aes_ct_bitslice_decrypt(ctx->rounds,ctx->sk,q);
    else br_aes_ct_bitslice_encrypt(ctx->rounds,ctx->sk,q);
    br_aes_ct_ortho(q);
    for(unsigned i=0;i<4;i++)for(unsigned j=0;j<4;j++)out[4*i+j]=(uint8_t)(q[2*i]>>(8*j));
    umi_ct_wipe(q,sizeof(q));
}
static int umi_ct_aes256(int decrypt,const uint8_t key[32],uint8_t block[16]) {
    struct umi_ct_key ctx;int rc=umi_ct_setkey(&ctx,key,32);
    if(!rc)umi_ct_block(&ctx,decrypt,block,block);
    umi_ct_wipe(&ctx,sizeof(ctx));return rc;
}
/* Complete-block XTS only: our data units are always exactly 4096 bytes. */
static void umi_xts_sector(const struct umi_ct_key *data,const struct umi_ct_key *tw,
                          int decrypt,uint64_t sector,const uint8_t *in,uint8_t *out) {
    uint8_t tweak[16]={0},block[16];
    for(unsigned i=0;i<8;i++)tweak[i]=(uint8_t)(sector>>(8*i));
    umi_ct_block(tw,0,tweak,tweak);
    for(unsigned n=0;n<4096;n+=16) {
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
