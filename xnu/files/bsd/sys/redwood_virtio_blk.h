/*
 * A virtio block device for QEMU: a disk image on the host becomes /dev/rvblk0 in the
 * guest.  Only with boot-arg virtio_blk=1.  See bsd/dev/redwood_virtio_blk.c.
 */
#ifndef _SYS_REDWOOD_VIRTIO_BLK_H_
#define _SYS_REDWOOD_VIRTIO_BLK_H_

void redwood_virtio_blk_init(void);

#endif /* _SYS_REDWOOD_VIRTIO_BLK_H_ */
