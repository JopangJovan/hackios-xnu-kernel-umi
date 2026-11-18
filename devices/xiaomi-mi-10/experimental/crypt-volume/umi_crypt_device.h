/* Included only in the isolated prepared hakios.ufs.c translation unit.
 * Synthetic RAM and bounded regular-file ciphertext disks. No partition access.
 * Live file writes use a checked, authenticated single-sector redo journal.
 */
#include <kern/kalloc.h>
#include <sys/kauth.h>
#include <corecrypto/cchmac.h>
#include <corecrypto/ccsha2.h>
static void uv_hmac(const uint8_t *k,size_t kn,const uint8_t *d,size_t dn,uint8_t o[32]) {
    cchmac(ccsha256_di(),kn,k,dn,d,o);
}
#include "umi_volume_crypto.h"
static LCK_GRP_DECLARE(uv_locks,"umi_crypt_lab");
static LCK_MTX_DECLARE(uv_lock,&uv_locks);
static struct {
    uint8_t *image;
    struct uv_keys keys;
    uint64_t filled;
    uint32_t mode,bsize,opens;
    pid_t owner;
    bool control,ready;
} uv;
static int uv_bmajor=-1,uv_cmajor=-1;
#include "umi_live_backend.h"
static int uv_root(void){return kauth_cred_getuid(kauth_cred_get())==0;}
static void uv_erase(void) {
    uv_live_close();
    if(uv.image){umi_ct_wipe(uv.image,UV_IMAGE);kfree_data(uv.image,UV_IMAGE);}
    uv.image=NULL;umi_ct_wipe(&uv.keys,sizeof(uv.keys));uv.ready=false;uv.mode=0;uv.filled=0;uv.bsize=512;
}
static int uv_open(dev_t d,int flags,int type,proc_t p) {
    (void)d;(void)flags;(void)type;(void)p;
    if(!uv_root())return EPERM;
    lck_mtx_lock(&uv_lock);int e=uv.ready?0:ENXIO;if(!e)uv.opens++;lck_mtx_unlock(&uv_lock);return e;
}
static int uv_close(dev_t d,int flags,int type,proc_t p) {
    (void)d;(void)flags;(void)type;(void)p;
    lck_mtx_lock(&uv_lock);if(uv.opens)uv.opens--;
    if(!uv.opens&&!uv.control)uv_erase();lck_mtx_unlock(&uv_lock);return 0;
}
static int uv_ctl_open(dev_t d,int flags,int type,proc_t p) {
    (void)d;(void)flags;(void)type;(void)p;
    if(!uv_root())return EPERM;
    lck_mtx_lock(&uv_lock);int e=uv.control||uv.opens?EBUSY:0;
    if(!e){uv.control=true;uv.owner=proc_selfpid();}lck_mtx_unlock(&uv_lock);return e;
}
static int uv_ctl_close(dev_t d,int flags,int type,proc_t p) {
    (void)d;(void)flags;(void)type;(void)p;
    lck_mtx_lock(&uv_lock);uv.control=false;uv.owner=0;
    if(!uv.opens)uv_erase();lck_mtx_unlock(&uv_lock);return 0;
}
/* Called while locked; authenticate before releasing plaintext to the VFS. */
static int uv_io(bool reading,uint64_t at,uint8_t *bytes,size_t n,uint8_t *bounce) {
    if(!uv.ready||(!uv.image&&!uv_live.vnode))return ENXIO;
    if(at>UV_BYTES||n>UV_BYTES-at)return EINVAL;
    uint8_t *temporary=uv_live.vnode?kalloc_data(UV_RECORD,Z_WAITOK):NULL;
    if(uv_live.vnode&&!temporary)return ENOMEM;
    int e=0;
    while(n&&!e) {
        uint64_t sector=at/UV_SECTOR;size_t within=at%UV_SECTOR,take=MIN(n,UV_SECTOR-within);
        uint8_t *record=temporary?temporary:uv.image+UV_HEADER+sector*UV_RECORD;
        if(temporary)e=uv_live_record_read(sector,record);
        if(!e&&uv_decrypt(&uv.keys,sector,record,bounce))e=EIO;
        if(e)break;
        if(reading)memcpy(bytes,bounce+within,take);
        else {memcpy(bounce+within,bytes,take);uv_encrypt(&uv.keys,sector,bounce,record);if(temporary)e=uv_live_commit(sector,record);}
        at+=take;bytes+=take;n-=take;
    }
    if(temporary){umi_ct_wipe(temporary,UV_RECORD);kfree_data(temporary,UV_RECORD);}return e;
}

static void uv_strategy(buf_t bp) {
    uint8_t *bounce=kalloc_data(UV_SECTOR,Z_WAITOK);caddr_t bytes=NULL;int e=0;
    uint64_t n=buf_count(bp);bool reading=(buf_flags(bp)&B_READ)!=0;
    buf_setresid(bp,buf_count(bp));
    if(!bounce)e=ENOMEM;
    else if(buf_blkno(bp)<0)e=EINVAL;
    else e=buf_map(bp,&bytes);
    if(!e) {
        lck_mtx_lock(&uv_lock);
        uint64_t block=(uint64_t)buf_blkno(bp);
        if(block>UV_BYTES/uv.bsize)e=EINVAL;
        else e=uv_io(reading,block*uv.bsize,(uint8_t *)bytes,(size_t)n,bounce);
        if(e&&reading)memset(bytes,0,(size_t)n);
        lck_mtx_unlock(&uv_lock);buf_unmap(bp);
    }
    if(bounce){umi_ct_wipe(bounce,UV_SECTOR);kfree_data(bounce,UV_SECTOR);}
    if(e)buf_seterror(bp,e);else buf_setresid(bp,0);buf_biodone(bp);
}
static int uv_ioctl(dev_t d,u_long cmd,caddr_t data,int flags,proc_t p) {
    (void)d;(void)flags;(void)p;int e=0;
    lck_mtx_lock(&uv_lock);
    if(!uv.ready){e=ENXIO;goto done;}
    switch(cmd) {
    case DKIOCGETBLOCKSIZE:*(uint32_t *)data=uv.bsize;break;
    case DKIOCSETBLOCKSIZE:
        if(*(uint32_t *)data<512||*(uint32_t *)data>UV_SECTOR||(*(uint32_t *)data&(*(uint32_t *)data-1)))e=EINVAL;
        else uv.bsize=*(uint32_t *)data;break;
    case DKIOCGETPHYSICALBLOCKSIZE:*(uint32_t *)data=UV_SECTOR;break;
    case DKIOCGETBLOCKCOUNT:*(uint64_t *)data=UV_BYTES/uv.bsize;break;
    case DKIOCISWRITABLE:*(uint32_t *)data=1;break;
    case DKIOCGETMAXBLOCKCOUNTREAD:case DKIOCGETMAXBLOCKCOUNTWRITE:*(uint64_t *)data=65536/uv.bsize;break;
    case DKIOCSYNCHRONIZE:case DKIOCSYNCHRONIZECACHE:
        if(uv_live.vnode)e=uv_live.poisoned?EIO:uv_live_sync();break;
    default:e=ENOTTY;break;
    }
done:lck_mtx_unlock(&uv_lock);return e;
}
static int uv_psize(dev_t d){(void)d;return (int)uv.bsize;}
static int uv_ctl_ioctl(dev_t d,u_long cmd,caddr_t data,int flags,proc_t p) {
    (void)d;(void)flags;(void)p;int e=0;
    if(!uv_root())return EPERM;
    lck_mtx_lock(&uv_lock);
    if(!uv.control||uv.owner!=proc_selfpid()){e=EPERM;goto done;}
    if(cmd==UV_LIVE_STATS){memcpy(data,&uv_live.stats,sizeof(uv_live.stats));goto done;}
    if(uv.opens){e=EBUSY;goto done;}
    if(cmd==UV_LIVE_ATTACH){e=uv_live_attach_file((struct uv_live_attach *)data);goto done;}
    if(cmd==UV_LIVE_INJECT){e=uv_live_inject_torn_record();goto done;}
    if(cmd==UV_CLEAR){uv_erase();goto done;}
    if(cmd==UV_ATTACH) {
        struct uv_attach *a=(struct uv_attach *)data;
        if(uv.image||uv_live.vnode||a->reserved||(a->mode!=1&&a->mode!=2)){e=EINVAL;goto done;}
        uv.image=kalloc_data(UV_IMAGE,Z_WAITOK|Z_ZERO);if(!uv.image){e=ENOMEM;goto done;}
        uv_keys_init(&uv.keys,a->master,a->uuid);uv.mode=a->mode;uv.bsize=512;
        if(a->mode==1)uv_header(&uv.keys,uv.image);
        /* ioctl's copied secret must not survive dispatch. */
        umi_ct_wipe(a->master,sizeof(a->master));goto done;
    }
    if(cmd==UV_FINISH) {
        if(!uv.image||uv.ready||uv.filled!=(uv.mode==1?UV_BYTES:UV_IMAGE)){e=EINVAL;goto done;}
        if(!uv_header_ok(&uv.keys,uv.image)){e=EACCES;goto bad;}
        for(uint64_t i=0;i<UV_BYTES/UV_SECTOR;i++)
            if(!uv_auth(&uv.keys,i,uv.image+UV_HEADER+i*UV_RECORD)){e=EACCES;goto bad;}
        uv.ready=true;printf("UMI crypt lab: authenticated 16 MiB ciphertext disk ready\n");goto done;
bad:   uv_erase();goto done;
    }
    e=ENOTTY;
done:
    if(cmd==UV_ATTACH)umi_ct_wipe(((struct uv_attach *)data)->master,32);
    if(cmd==UV_LIVE_ATTACH)umi_ct_wipe(((struct uv_live_attach *)data)->master,32);
    lck_mtx_unlock(&uv_lock);return e;
}
static int uv_ctl_write(dev_t d,struct uio *uio,int flags) {
    (void)d;(void)flags;int e=0;
    if(!uv_root())return EPERM;
    uint8_t *bounce=kalloc_data(UV_SECTOR,Z_WAITOK);if(!bounce)return ENOMEM;
    while(!e&&uio_resid(uio)>0) {
        off_t at=uio_offset(uio);size_t n=(size_t)MIN(uio_resid(uio),UV_SECTOR);
        e=uiomove((caddr_t)bounce,(int)n,uio);if(e)break;
        lck_mtx_lock(&uv_lock);
        if(!uv.control||uv.owner!=proc_selfpid())e=EPERM;
        else if(!uv.image||uv.ready||uv.opens||at<0||(uint64_t)at!=uv.filled)e=EINVAL;
        else if(uv.mode==1) {
            if(n!=UV_SECTOR||(uint64_t)at>UV_BYTES-n)e=EINVAL;
            else {uv_encrypt(&uv.keys,(uint64_t)at/UV_SECTOR,bounce,uv.image+UV_HEADER+((uint64_t)at/UV_SECTOR)*UV_RECORD);uv.filled+=n;}
        } else {
            if((uint64_t)at>UV_IMAGE-n)e=EINVAL;
            else {memcpy(uv.image+at,bounce,n);uv.filled+=n;}
        }
        lck_mtx_unlock(&uv_lock);
        if(e){uio_setoffset(uio,at);uio_setresid(uio,uio_resid(uio)+n);}
    }
    umi_ct_wipe(bounce,UV_SECTOR);kfree_data(bounce,UV_SECTOR);return e;
}
static int uv_ctl_read(dev_t d,struct uio *uio,int flags) {
    (void)d;(void)flags;int e=0;
    if(!uv_root())return EPERM;
    uint8_t *bounce=kalloc_data(UV_SECTOR,Z_WAITOK);if(!bounce)return ENOMEM;
    while(!e&&uio_resid(uio)>0) {
        off_t at=uio_offset(uio);size_t n=(size_t)MIN(uio_resid(uio),UV_SECTOR);
        lck_mtx_lock(&uv_lock);
        if(!uv.control||uv.owner!=proc_selfpid())e=EPERM;
        else if(!uv.ready||!uv.image||uv.opens||at<0||(uint64_t)at>UV_IMAGE)e=EINVAL;
        else {n=MIN(n,UV_IMAGE-(size_t)at);memcpy(bounce,uv.image+at,n);}
        lck_mtx_unlock(&uv_lock);
        if(e||!n)break;e=uiomove((caddr_t)bounce,(int)n,uio);
    }
    umi_ct_wipe(bounce,UV_SECTOR);kfree_data(bounce,UV_SECTOR);return e;
}
static const struct bdevsw uv_bdev={.d_open=uv_open,.d_close=uv_close,.d_strategy=uv_strategy,.d_ioctl=uv_ioctl,
    .d_dump=eno_dump,.d_psize=uv_psize,.d_type=D_DISK};
static const struct cdevsw uv_cdev={.d_open=uv_ctl_open,.d_close=uv_ctl_close,.d_read=uv_ctl_read,.d_write=uv_ctl_write,
    .d_ioctl=uv_ctl_ioctl,.d_stop=eno_stop,.d_reset=eno_reset,.d_ttys=NULL,.d_select=eno_select,.d_mmap=eno_mmap,
    .d_strategy=eno_strat,.d_reserved_1=eno_getc,.d_reserved_2=eno_putc,.d_type=0};
static void umi_crypt_lab_init(void) {
    int enabled=0;if(uv_bmajor>=0||!PE_parse_boot_argn("umi_aks_lab",&enabled,sizeof(enabled))||enabled!=1)return;
    uv.bsize=512;uv_bmajor=bdevsw_add(-1,&uv_bdev);uv_cmajor=cdevsw_add(-1,&uv_cdev);
    if(uv_bmajor<0||uv_cmajor<0){printf("UMI crypt lab: registration failed\n");return;}
    devfs_make_node(makedev(uv_bmajor,0),DEVFS_BLOCK,0,0,0600,"umi_crypt_lab");
    devfs_make_node(makedev(uv_cmajor,0),DEVFS_CHAR,0,0,0600,"umi_crypt_ctl");
    printf("UMI crypt lab: root-only RAM ciphertext device registered\n");
}
