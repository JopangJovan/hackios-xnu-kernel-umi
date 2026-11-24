/* UMI checked physical RAW10 backend. Retain power and focus until close;
 * each frame still uses a bounded sensor/DMA transfer. Rear AF and fixed-focus front have distinct formats. */
struct umi_camera_format {
    unsigned width,height,stride,raw_stride,line_ns,frame_min;
};
static const struct umi_camera_format umi_rear_format={1504,1128,1880,3760,14020,2373};
static const struct umi_camera_format umi_front_format={1296,970,1620,3248,16080,2064};
static const struct umi_camera_format *umi_camera_format(void)
{
    return umi_cam_sensor==2?&umi_front_format:&umi_rear_format;
}
static void
umi_camera_header(struct hakios_camera_frame *f,uint64_t sequence,uint64_t time,
    uint32_t lines,uint32_t gain,uint32_t frame_lines)
{
    const struct umi_camera_format *fmt=umi_camera_format();
    memset(f,0,sizeof(*f));memcpy(f->magic,HAKIOS_CAMERA_MAGIC,8);
    f->header_bytes=sizeof(*f);f->width=fmt->width;f->height=fmt->height;f->stride=fmt->stride;
    f->format=1;f->bayer=1 /* CamX CFA2 is GRBG; camera ABI code1 */;f->black=64;f->sensor=umi_cam_sensor;
    f->sequence=sequence;f->time=time;f->exposure_lines=lines;
    f->line_ns=fmt->line_ns;f->gain=gain;f->gain_unity=32;f->gain_max=512;
    f->frame_lines=frame_lines;f->frame_lines_min=fmt->frame_min;f->frame_lines_max=65520;
    f->margin=8;f->focus=umi_cam_sensor==2?HAKIOS_CAMERA_NO_FOCUS:umi_cam_frame_focus;
}
/* Take a 2x2 Bayer cell from each 4x4 source tile. This preserves colour
 * phase while retaining the full field of view at half the dimensions. */
static void
umi_camera_reduce_raw10(const uint8_t *src,uint8_t *dst)
{
    const struct umi_camera_format *fmt=umi_camera_format();
    for(unsigned y=0;y<fmt->height;y++) {
        unsigned sy=(y/2)*4+(y&1);
        for(unsigned x=0;x<fmt->width;x+=4) {
            unsigned at=y*fmt->stride+(x/4)*5, low=0;
            for(unsigned k=0;k<4;k++) {
                unsigned sx=((x+k)/2)*4+((x+k)&1), in=sy*fmt->raw_stride+(sx/4)*5;
                unsigned value=((unsigned)src[in+(sx&3)]<<2)|((src[in+4]>>(2*(sx&3)))&3);
                dst[at+k]=(uint8_t)(value>>2);low|=(value&3)<<(2*k);
            }
            dst[at+4]=(uint8_t)low;
        }
    }
}
/* DEVICE METHODS */
static LCK_GRP_DECLARE(umi_cam_group,"umi-camera");
static LCK_MTX_DECLARE(umi_cam_lock,&umi_cam_group);
static uint64_t umi_cam_sequence;
static uint8_t *umi_cam_preview;
static int
umi_cam_open(__unused dev_t d,__unused int f,__unused int t,__unused struct proc *p)
{ return 0; }
static int
umi_cam_close(__unused dev_t d,__unused int f,__unused int t,__unused struct proc *p)
{
    lck_mtx_lock(&umi_cam_lock);
    umi_camera_ios_release();
    lck_mtx_unlock(&umi_cam_lock);
    return 0;
}
static int
umi_cam_read(__unused dev_t d,struct uio *uio,__unused int flags)
{
    if(uio_resid(uio)==0)return 0;
    if(uio_resid(uio)<(user_ssize_t)sizeof(struct hakios_camera_frame))return EINVAL;
    lck_mtx_lock(&umi_cam_lock);
    const struct umi_camera_format *fmt=umi_camera_format();
    unsigned frame_bytes=fmt->stride*fmt->height;
    user_ssize_t room=uio_resid(uio);
    /* A stale small buffer after switching sensors cannot hold the new header
     * plus RAW frame. Reject it before copying a misleading larger header. */
    if(room!=(user_ssize_t)sizeof(struct hakios_camera_frame) &&
        room<(user_ssize_t)(sizeof(struct hakios_camera_frame)+frame_bytes)) {
        lck_mtx_unlock(&umi_cam_lock);return EMSGSIZE;
    }
    if(!umi_cam_preview)umi_cam_preview=kalloc_data(2120640,Z_WAITOK);
    if(!umi_cam_preview){lck_mtx_unlock(&umi_cam_lock);return ENOMEM;}
    int error=umi_camera_ios_capture();
    if(!error) {
        const uint8_t *frame=umi_camera_sensor_data();
        if(!frame)error=EIO;
        else {
            umi_camera_reduce_raw10(frame,umi_cam_preview);
            struct hakios_camera_frame head;
            umi_camera_header(&head,++umi_cam_sequence,mach_absolute_time(),umi_cam_lines,umi_cam_gain,umi_cam_frame_lines);
            error=uiomove((caddr_t)&head,sizeof(head),uio);
            if(!error&&uio_resid(uio)>0) {
                user_ssize_t bytes=uio_resid(uio);
                if(bytes>frame_bytes)bytes=frame_bytes;
                error=uiomove((caddr_t)umi_cam_preview,(int)bytes,uio);
            }
        }
    }
    if(error)umi_camera_ios_release();
    lck_mtx_unlock(&umi_cam_lock);
    return error;
}
static int
umi_cam_write(__unused dev_t d,struct uio *uio,__unused int flags)
{
    struct hakios_camera_command c;
    if(uio_resid(uio)!=(user_ssize_t)sizeof(c))return EINVAL;
    int error=uiomove((caddr_t)&c,sizeof(c),uio);
    if(error)return error;
    if(c.bytes)return ENOTSUP;
    switch(c.command) {
    case HAKIOS_CAMERA_EXPOSURE:
        lck_mtx_lock(&umi_cam_lock);
        unsigned min_frame=umi_camera_format()->frame_min;
        umi_cam_frame_lines=c.c<min_frame?min_frame:c.c>65520?65520:c.c;
        umi_cam_lines=c.a<1?1:c.a>umi_cam_frame_lines-8?umi_cam_frame_lines-8:c.a;
        umi_cam_gain=c.b<32?32:c.b>512?512:c.b;
        lck_mtx_unlock(&umi_cam_lock);return 0;
    case HAKIOS_CAMERA_SENSOR:
        if(c.a!=1&&c.a!=2)return ENODEV;
        lck_mtx_lock(&umi_cam_lock);
        if(umi_cam_sensor!=c.a) {
            umi_camera_ios_release();
            umi_cam_sensor=c.a;
            umi_cam_frame_lines=umi_camera_format()->frame_min;
            umi_cam_lines=c.a==2?1000:1600;umi_cam_gain=128;
            umi_cam_frame_focus=HAKIOS_CAMERA_NO_FOCUS;
        }
        lck_mtx_unlock(&umi_cam_lock);return 0;
    case HAKIOS_CAMERA_FOCUS:
        if(c.a>1023)return EINVAL;
        lck_mtx_lock(&umi_cam_lock);
        if(umi_cam_sensor==2){lck_mtx_unlock(&umi_cam_lock);return ENOTSUP;}
        umi_cam_focus=c.a;
        lck_mtx_unlock(&umi_cam_lock);return 0;
    default:return ENOTSUP;
    }
}
static const struct cdevsw umi_cam_ops={
    .d_open=umi_cam_open,.d_close=umi_cam_close,.d_read=umi_cam_read,.d_write=umi_cam_write,
    .d_ioctl=eno_ioctl,.d_stop=eno_stop,.d_reset=eno_reset,.d_ttys=NULL,
    .d_select=eno_select,.d_mmap=eno_mmap,.d_strategy=eno_strat,.d_type=0
};
void
umi_camera_serve_ios(void)
{
    int major=cdevsw_add(-1,&umi_cam_ops);
    if(major<0||devfs_make_node(makedev(major,0),DEVFS_CHAR,UID_ROOT,GID_WHEEL,0666,"hakios-camera")==NULL) {
        hakios_tl("UMI camera: could not publish physical camera device\n");return;
    }
    hakios_tl("UMI camera: /dev/hakios-camera ready: physical S5KHMX rear / S5K3T2 front session preview\n");
}
