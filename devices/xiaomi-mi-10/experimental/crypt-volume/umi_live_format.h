/* Single-record redo journal. The caller orders write/sync/main/sync/clear/sync.
 * No rollback prevention; a snapshot from the same volume can be replayed.
 */
#ifndef UMI_LIVE_FORMAT_H
#define UMI_LIVE_FORMAT_H
static uint64_t uv_get64(const uint8_t *p) {
    uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;
}
static void uv_journal_make(const struct uv_keys *keys,uint64_t sector,
                            const uint8_t record[UV_RECORD],uint8_t journal[UV_JOURNAL]) {
    memset(journal,0,UV_JOURNAL);memcpy(journal,"UVJNL001",8);
    uv_le64(journal+16,sector);memcpy(journal+32,keys->uuid,16);
    memcpy(journal+64,record,UV_RECORD);
    uv_hmac(keys->mac,32,journal,UV_JOURNAL-32,journal+UV_JOURNAL-32);
}
static int uv_journal_valid(const struct uv_keys *keys,const uint8_t journal[UV_JOURNAL],uint64_t *sector) {
    uint8_t tag[32];uv_hmac(keys->mac,32,journal,UV_JOURNAL-32,tag);
    int good=uv_equal(tag,journal+UV_JOURNAL-32,32);umi_ct_wipe(tag,sizeof(tag));
    if(!good||memcmp(journal,"UVJNL001",8)||memcmp(journal+32,keys->uuid,16))return 0;
    *sector=uv_get64(journal+16);
    return *sector<UV_BYTES/UV_SECTOR&&uv_auth(keys,*sector,journal+64);
}
#endif
