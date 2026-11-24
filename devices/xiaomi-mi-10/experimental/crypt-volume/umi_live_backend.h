/* Included after the test disk state. Bounded regular file on existing iOS HFS.
 * All references and buffers are cleared on detach. Never opens device nodes.
 */
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include "umi_live_format.h"
static struct {
    vnode_t vnode;
    bool poisoned;
    struct uv_live_stats stats;
} uv_live;
static int uv_live_exact(bool writing,uint64_t at,void *data,size_t size) {
    if(!uv_live.vnode||at>UV_LIVE_BYTES||size>UV_LIVE_BYTES-at||size>UV_JOURNAL)return EINVAL;
    vfs_context_t ctx=vfs_context_kernel();int residual=0;
    int e=vn_rdwr(writing?UIO_WRITE:UIO_READ,uv_live.vnode,data,(int)size,(off_t)at,
        UIO_SYSSPACE,IO_NOCACHE,vfs_context_ucred(ctx),&residual,vfs_context_proc(ctx));
    return e?e:residual?EIO:0;
}
static int uv_live_sync(void) {
    if(!uv_live.vnode)return ENXIO;
    return VNOP_FSYNC(uv_live.vnode,MNT_WAIT,vfs_context_kernel());
}
static void uv_live_close(void) {
    if(uv_live.vnode)vnode_close(uv_live.vnode,FREAD|FWRITE|FWASWRITTEN,vfs_context_kernel());
    memset(&uv_live,0,sizeof(uv_live));
}
static int uv_live_record_read(uint64_t sector,uint8_t record[UV_RECORD]) {
    if(uv_live.poisoned)return EIO;
    return uv_live_exact(false,UV_HEADER+sector*UV_RECORD,record,UV_RECORD);
}
static int uv_live_commit(uint64_t sector,const uint8_t record[UV_RECORD]) {
    if(uv_live.poisoned)return EIO;
    uint8_t *journal=kalloc_data(UV_JOURNAL,Z_WAITOK|Z_ZERO);if(!journal)return ENOMEM;
    uv_journal_make(&uv.keys,sector,record,journal);
    int e=uv_live_exact(true,UV_IMAGE,journal,UV_JOURNAL);
    if(!e)e=uv_live_sync();
    if(!e)e=uv_live_exact(true,UV_HEADER+sector*UV_RECORD,(void *)record,UV_RECORD);
    if(!e)e=uv_live_sync();
    if(!e){memset(journal,0,UV_JOURNAL);e=uv_live_exact(true,UV_IMAGE,journal,UV_JOURNAL);}
    if(!e)e=uv_live_sync();
    if(e)uv_live.poisoned=true;else uv_live.stats.writes++;
    umi_ct_wipe(journal,UV_JOURNAL);kfree_data(journal,UV_JOURNAL);return e;
}
static int uv_live_path_ok(const char path[192]) {
    static const char prefix[]="/private/var/tmp/umi-crypt-live-";
    const size_t n=sizeof(prefix)-1;
    if(memcmp(path,prefix,n))return 0;
    for(size_t i=n;i<n+12;i++)if(!((path[i]>='0'&&path[i]<='9')||(path[i]>='a'&&path[i]<='f')))return 0;
    return !memcmp(path+n+12,"/volume.live",13);
}
static int uv_live_attach_file(struct uv_live_attach *a) {
    if(uv.image||uv_live.vnode||!uv_live_path_ok(a->path))return EINVAL;
    char parent[192];memcpy(parent,a->path,sizeof(parent));parent[sizeof("/private/var/tmp/umi-crypt-live-")-1+12]=0;
    vnode_t dir=NULLVP;vfs_context_t ctx=vfs_context_kernel();struct vnode_attr attr;int e;
    e=vnode_lookup(parent,VNODE_LOOKUP_NOFOLLOW,&dir,ctx);if(e)return e;
    VATTR_INIT(&attr);VATTR_WANTED(&attr,va_uid);VATTR_WANTED(&attr,va_mode);
    e=vnode_getattr(dir,&attr,ctx);
    if(!e&&(vnode_vtype(dir)!=VDIR||!VATTR_IS_SUPPORTED(&attr,va_uid)||!VATTR_IS_SUPPORTED(&attr,va_mode)||attr.va_uid||(attr.va_mode&07777)!=0700))e=EPERM;
    vnode_put(dir);if(e)return e;
    e=vnode_open(a->path,FREAD|FWRITE|O_NOFOLLOW,0,VNODE_LOOKUP_NOFOLLOW,&uv_live.vnode,ctx);if(e)return e;
    VATTR_INIT(&attr);VATTR_WANTED(&attr,va_uid);VATTR_WANTED(&attr,va_mode);VATTR_WANTED(&attr,va_nlink);VATTR_WANTED(&attr,va_data_size);
    e=vnode_getattr(uv_live.vnode,&attr,ctx);
    if(!e&&(vnode_vtype(uv_live.vnode)!=VREG||!VATTR_IS_SUPPORTED(&attr,va_uid)||!VATTR_IS_SUPPORTED(&attr,va_mode)||
        !VATTR_IS_SUPPORTED(&attr,va_nlink)||!VATTR_IS_SUPPORTED(&attr,va_data_size)||attr.va_uid||(attr.va_mode&07777)!=0600||attr.va_nlink!=1||attr.va_data_size!=UV_LIVE_BYTES))e=EPERM;
    if(!e&&strcmp(vfs_statfs(vnode_mount(uv_live.vnode))->f_mntfromname,"/dev/ufs_data"))e=EPERM;
    if(e)goto bad;
    uv_keys_init(&uv.keys,a->master,a->uuid);
    uint8_t header[UV_HEADER];e=uv_live_exact(false,0,header,sizeof(header));
    if(!e&&!uv_header_ok(&uv.keys,header))e=EACCES;
    if(e)goto bad;
    uint8_t *journal=kalloc_data(UV_JOURNAL,Z_WAITOK|Z_ZERO),*record=kalloc_data(UV_RECORD,Z_WAITOK);
    if(!journal||!record){if(journal)kfree_data(journal,UV_JOURNAL);if(record)kfree_data(record,UV_RECORD);e=ENOMEM;goto bad;}
    uint64_t pending=UINT64_MAX;e=uv_live_exact(false,UV_IMAGE,journal,UV_JOURNAL);
    bool valid=!e&&uv_journal_valid(&uv.keys,journal,&pending);unsigned nonzero=0;
    for(size_t i=0;i<UV_JOURNAL;i++)nonzero|=journal[i];
    /* A torn prepare/clear is discardable only when every main record is valid.
     * A valid redo journal can replace precisely its authenticated target record.
     */
    for(uint64_t i=0;!e&&i<UV_BYTES/UV_SECTOR;i++) {
        if(valid&&i==pending)continue;
        e=uv_live_exact(false,UV_HEADER+i*UV_RECORD,record,UV_RECORD);
        if(!e&&!uv_auth(&uv.keys,i,record))e=EACCES;
    }
    if(!e&&valid){e=uv_live_exact(true,UV_HEADER+pending*UV_RECORD,journal+64,UV_RECORD);if(!e)e=uv_live_sync();if(!e)uv_live.stats.replayed++;}
    if(!e&&nonzero){memset(journal,0,UV_JOURNAL);e=uv_live_exact(true,UV_IMAGE,journal,UV_JOURNAL);if(!e)e=uv_live_sync();if(!e&&!valid)uv_live.stats.discarded++;}
    umi_ct_wipe(journal,UV_JOURNAL);umi_ct_wipe(record,UV_RECORD);kfree_data(journal,UV_JOURNAL);kfree_data(record,UV_RECORD);
    if(e)goto bad;
    uv.ready=true;uv.bsize=512;uv.mode=3;
    printf("UMI crypt live: ready; replayed=%llu discarded=%llu\n",uv_live.stats.replayed,uv_live.stats.discarded);return 0;
bad:uv_live_close();umi_ct_wipe(&uv.keys,sizeof(uv.keys));return e;
}
/* Explicit synthetic recovery fixture; only unmounted lab volumes may enter.
 * Persist a valid redo record, then half-corrupt its main ciphertext record.
 * Leave recovery for the next attach/boot; never call on real data.
 */
static int uv_live_inject_torn_record(void) {
    if(!uv_live.vnode||uv_live.poisoned||!uv.ready)return EINVAL;
    uint8_t *journal=kalloc_data(UV_JOURNAL,Z_WAITOK|Z_ZERO),*record=kalloc_data(UV_RECORD,Z_WAITOK);
    if(!journal||!record){if(journal)kfree_data(journal,UV_JOURNAL);if(record)kfree_data(record,UV_RECORD);return ENOMEM;}
    int e=uv_live_record_read(0,record);
    if(!e&&!uv_auth(&uv.keys,0,record))e=EACCES;
    if(!e){uv_journal_make(&uv.keys,0,record,journal);e=uv_live_exact(true,UV_IMAGE,journal,UV_JOURNAL);}
    if(!e)e=uv_live_sync();
    if(!e){for(unsigned i=0;i<2048;i++)record[i]^=0x5a;e=uv_live_exact(true,UV_HEADER,record,2048);}
    if(!e)e=uv_live_sync();
    uv_live.poisoned=true;uv.ready=false;
    umi_ct_wipe(journal,UV_JOURNAL);umi_ct_wipe(record,UV_RECORD);kfree_data(journal,UV_JOURNAL);kfree_data(record,UV_RECORD);
    if(!e)printf("UMI crypt live: synthetic torn main record persisted; redo required\n");return e;
}
